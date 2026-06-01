
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_stream.h>


static char *ngx_stream_modbus_proxy_location(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char *ngx_stream_modbus_proxy_host(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char *ngx_stream_modbus_proxy_timeout(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_stream_modbus_proxy_preread(ngx_stream_session_t *s);
static void ngx_stream_modbus_proxy_timeout_handler(ngx_event_t *ev);
static void ngx_stream_modbus_proxy_cleanup_timer(void *data);
static ngx_int_t ngx_stream_modbus_proxy_variable(ngx_stream_session_t *s,
                                              ngx_stream_variable_value_t *v,
                                              uintptr_t data);
static void *ngx_stream_modbus_proxy_create_srv_conf(ngx_conf_t *cf);
static char *ngx_stream_modbus_proxy_merge_srv_conf(ngx_conf_t *cf, void *parent, void *child);
static ngx_int_t ngx_stream_modbus_proxy_postconfiguration(ngx_conf_t *cf);



// Location conf
typedef struct
{
    ngx_uint_t slave_id;
    ngx_str_t proxy_pass;
    ngx_msec_t timeout;   // absolute max session duration, 0 = unlimited
} ngx_stream_modbus_proxy_loc_conf_t;

// server conf
typedef struct
{
    ngx_array_t *locations;
    ngx_stream_modbus_proxy_loc_conf_t *default_location;

} ngx_stream_modbus_proxy_srv_conf_t;

// modbus block
typedef struct
{
    ngx_stream_modbus_proxy_loc_conf_t *loc_conf;
} ngx_stream_modbus_proxy_ctx_t;

static ngx_command_t ngx_stream_modbus_proxy_commands[] = {
    {ngx_string("modbus"),
     NGX_STREAM_SRV_CONF | NGX_CONF_BLOCK | NGX_CONF_TAKE1,
     ngx_stream_modbus_proxy_location,
     NGX_STREAM_SRV_CONF_OFFSET,
     0,
     NULL},

    {ngx_string("host"),
     NGX_STREAM_SRV_CONF | NGX_CONF_TAKE1,
     ngx_stream_modbus_proxy_host,
     0,
     0,
     NULL},

    {ngx_string("timeout"),
     NGX_STREAM_SRV_CONF | NGX_CONF_TAKE1,
     ngx_stream_modbus_proxy_timeout,
     0,
     0,
     NULL},

    ngx_null_command};

static ngx_stream_module_t ngx_stream_modbus_proxy_module_ctx = {
    NULL,                                 /* preconfiguration */
    ngx_stream_modbus_proxy_postconfiguration, /* postconfiguration */
    NULL,                                 /* create main configuration */
    NULL,                                 /* merge main configuration */
    ngx_stream_modbus_proxy_create_srv_conf,   /* create server configuration */
    ngx_stream_modbus_proxy_merge_srv_conf     /* merge server configuration */
};
ngx_module_t ngx_stream_modbus_proxy_module = {
    NGX_MODULE_V1,
    &ngx_stream_modbus_proxy_module_ctx, /* module context */
    ngx_stream_modbus_proxy_commands,    /*module directives*/
    NGX_STREAM_MODULE,              /* module type */
    NULL,                           /* init master */
    NULL,                           /* init module */
    NULL,                           /* init process */
    NULL,                           /* init thread */
    NULL,                           /* exit thread */
    NULL,                           /* exit process */
    NULL,                           /* exit master */
    NGX_MODULE_V1_PADDING};

// directive

static ngx_stream_modbus_proxy_loc_conf_t *ngx_stream_modbus_proxy_find_location(ngx_stream_modbus_proxy_srv_conf_t *mgcf, ngx_uint_t slave_id);


static char *ngx_stream_modbus_proxy_host(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    // Inside a modbus { } block cf->ctx is swapped to our own ctx (see
    // ngx_stream_modbus_proxy_location), so read the location directly from there
    // rather than from the `conf` argument the config engine computes.
    ngx_stream_modbus_proxy_ctx_t *ctx = cf->ctx;
    ngx_stream_modbus_proxy_loc_conf_t *mlcf = ctx->loc_conf;
    ngx_str_t *value = cf->args->elts;

    mlcf->proxy_pass = value[1];
    ngx_conf_log_error(NGX_LOG_INFO, cf, 0,
                       "parse config: modbus location slave_id=%ui: host %V",
                       mlcf->slave_id, &mlcf->proxy_pass);

    return NGX_CONF_OK;
}


// "timeout <time>;" inside a modbus { } block: absolute max session duration.
static char *ngx_stream_modbus_proxy_timeout(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_stream_modbus_proxy_ctx_t *ctx = cf->ctx;
    ngx_stream_modbus_proxy_loc_conf_t *mlcf = ctx->loc_conf;
    ngx_str_t *value = cf->args->elts;
    ngx_int_t t;

    t = ngx_parse_time(&value[1], 0);   // 0 -> result in milliseconds
    if (t == NGX_ERROR) {
        return "has an invalid timeout value";
    }

    mlcf->timeout = (ngx_msec_t) t;

    ngx_log_debug2(NGX_LOG_WARN, cf->log, 0,
                       "gateway timeout: modbus slave_id=%ui: timeout %M ms",
                       mlcf->slave_id, mlcf->timeout);

    return NGX_CONF_OK;
}


// Fires when a session outlives its backend's configured timeout: close it.
static void ngx_stream_modbus_proxy_timeout_handler(ngx_event_t *ev)
{
    ngx_stream_session_t  *s = ev->data;
    ngx_connection_t      *c = s->connection;
    ngx_stream_upstream_t *u = s->upstream;

    ngx_log_debug0(NGX_LOG_INFO, c->log, 0,
                  "modbus_proxy: session timeout reached, closing connection");

    if (u != NULL && u->peer.connection != NULL) {
        c->read->timedout = 1;
        c->read->handler(c->read);
        return;
    }

    // No upstream connection yet: closing the client session is enough.
    ngx_stream_finalize_session(s, NGX_STREAM_OK);
}


// Pool cleanup: remove the timer from the event tree if the session ends
// before the timeout fires, so the tree never points at freed memory.
static void ngx_stream_modbus_proxy_cleanup_timer(void *data)
{
    ngx_event_t *ev = data;

    ngx_log_debug0(NGX_LOG_DEBUG_STREAM, ev->log, 0,
                   "modbus_proxy: cleanup session timer");

    if (ev->timer_set) {
        ngx_del_timer(ev);
    }
}



// Handler for modbus block: modbus <slave_id> { ... }
static char *ngx_stream_modbus_proxy_location(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_stream_modbus_proxy_srv_conf_t *mgcf = conf;
    ngx_str_t *value = cf->args->elts;
    ngx_stream_modbus_proxy_loc_conf_t *loc_conf;
    ngx_stream_modbus_proxy_ctx_t *ctx;
    char *rv;

    // Create new location configuration
    loc_conf = ngx_pcalloc(cf->pool, sizeof(ngx_stream_modbus_proxy_loc_conf_t));
    if (loc_conf == NULL)
    {
        return NGX_CONF_ERROR;
    }

    loc_conf->proxy_pass.len = 0;
    loc_conf->proxy_pass.data = NULL;

    // Parse slave_id
    if (ngx_strcmp(value[1].data, "default") == 0)
    {
        loc_conf->slave_id = 0; // Default location
    }
    else
    {
        loc_conf->slave_id = ngx_atoi(value[1].data, value[1].len);
        if (loc_conf->slave_id == (ngx_uint_t)NGX_ERROR || loc_conf->slave_id > 255)
        {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "invalid modbus slave_id: %V", &value[1]);
            return NGX_CONF_ERROR;
        }
    }

    if (mgcf->locations == NULL)
    {
        mgcf->locations = ngx_array_create(cf->pool, 4, sizeof(ngx_stream_modbus_proxy_loc_conf_t));
        if (mgcf->locations == NULL)
        {
            return NGX_CONF_ERROR;
        }
    }

    // Set up context for parsing inside block
    ctx = ngx_pcalloc(cf->pool, sizeof(ngx_stream_modbus_proxy_ctx_t));
    if (ctx == NULL)
    {
        return NGX_CONF_ERROR;
    }

    ctx->loc_conf = loc_conf;

    // Save current context and set new one
    void *save = cf->ctx;
    cf->ctx = ctx;

    // Parse the block (the `host` directive fills in loc_conf->proxy_pass)
    rv = ngx_conf_parse(cf, NULL);

    // Restore context
    cf->ctx = save;

    if (rv != NGX_CONF_OK)
    {
        return rv;
    }

    // Store in array for later lookup, now that the block has been parsed and
    // proxy_pass is populated.
    ngx_stream_modbus_proxy_loc_conf_t *new_loc = ngx_array_push(mgcf->locations);
    if (new_loc == NULL)
    {
        return NGX_CONF_ERROR;
    }
    *new_loc = *loc_conf;

    // If this is the default location, remember it. Point at the standalone
    // loc_conf (stable on cf->pool) rather than the array element, since later
    // ngx_array_push calls may reallocate the array.
    if (loc_conf->slave_id == 0)
    {
        mgcf->default_location = loc_conf;
    }

    return NGX_CONF_OK;
}

// Find location by slave_id
static ngx_stream_modbus_proxy_loc_conf_t *ngx_stream_modbus_proxy_find_location(ngx_stream_modbus_proxy_srv_conf_t *mgcf, ngx_uint_t slave_id)
{
    ngx_uint_t i;
    ngx_stream_modbus_proxy_loc_conf_t *locations;

    if (mgcf->locations == NULL)
    {
        return mgcf->default_location;
    }

    locations = mgcf->locations->elts;

    // Exact match
    for (i = 0; i < mgcf->locations->nelts; i++)
    {
        if (locations[i].slave_id == slave_id)
        {
            return &locations[i];
        }
    }

    // Return default if configured
    return mgcf->default_location;
}

// Preread-phase handler: peek the Modbus TCP MBAP header to pick a backend.
//
// The MBAP header is 7 bytes; byte 6 is the unit (slave) id. We read it from
// the preread buffer that nginx fills for us, WITHOUT consuming it, so the
// full frame is still forwarded upstream by the proxy module. The chosen
// backend is stashed in the per-session ctx for the $modbus_backend variable.
static ngx_int_t ngx_stream_modbus_proxy_preread(ngx_stream_session_t *s)
{
    ngx_connection_t              *c = s->connection;
    ngx_stream_modbus_proxy_srv_conf_t *mgcf;
    ngx_stream_modbus_proxy_loc_conf_t *loc;
    ngx_stream_modbus_proxy_ctx_t      *ctx;
    ngx_uint_t                     slave_id;

    ctx = ngx_stream_get_module_ctx(s, ngx_stream_modbus_proxy_module);
    if (ctx != NULL) {
        // Backend already chosen on an earlier preread invocation.
        return NGX_OK;
    }

    mgcf = ngx_stream_get_module_srv_conf(s, ngx_stream_modbus_proxy_module);
    if (mgcf == NULL) {
        return NGX_OK;
    }

    // Wait until the 7-byte MBAP header is fully buffered.
    if (c->buffer == NULL || (size_t) (c->buffer->last - c->buffer->pos) < 7)
    {
        return NGX_AGAIN;
    }

    slave_id = c->buffer->pos[6];

    loc = ngx_stream_modbus_proxy_find_location(mgcf, slave_id);
    if (loc == NULL || loc->proxy_pass.len == 0) {
        ngx_log_error(NGX_LOG_ERR, c->log, 0,
                      "modbus_proxy: no backend for slave_id=%ui", slave_id);
        return NGX_STREAM_BAD_GATEWAY;
    }

    ctx = ngx_pcalloc(c->pool, sizeof(ngx_stream_modbus_proxy_ctx_t));
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    ctx->loc_conf = loc;
    ngx_stream_set_ctx(s, ctx, ngx_stream_modbus_proxy_module);

    ngx_log_debug2(NGX_LOG_DEBUG_STREAM, c->log, 0,
                  "modbus_proxy: slave_id=%ui -> %V",
                  slave_id, &loc->proxy_pass);

    // Arm the absolute session-duration timer for this backend, if configured.
    if (loc->timeout) {
        ngx_event_t        *tev;
        ngx_pool_cleanup_t *cln;

        tev = ngx_pcalloc(c->pool, sizeof(ngx_event_t));
        if (tev == NULL) {
            return NGX_ERROR;
        }

        tev->handler = ngx_stream_modbus_proxy_timeout_handler;
        tev->data = s;
        tev->log = c->log;

        // Ensure the timer is cancelled if the session closes on its own first.
        cln = ngx_pool_cleanup_add(c->pool, 0);
        if (cln == NULL) {
            return NGX_ERROR;
        }
        cln->handler = ngx_stream_modbus_proxy_cleanup_timer;
        cln->data = tev;

        ngx_add_timer(tev, loc->timeout);
    }

    return NGX_OK;
}

// $modbus_backend: returns the backend selected during the preread phase.
static ngx_int_t ngx_stream_modbus_proxy_variable(ngx_stream_session_t *s,
                                              ngx_stream_variable_value_t *v,
                                              uintptr_t data)
{
    ngx_stream_modbus_proxy_ctx_t *ctx;

    ctx = ngx_stream_get_module_ctx(s, ngx_stream_modbus_proxy_module);

    if (ctx == NULL || ctx->loc_conf == NULL
        || ctx->loc_conf->proxy_pass.len == 0)
    {
        v->not_found = 1;
        return NGX_OK;
    }

    v->len = ctx->loc_conf->proxy_pass.len;
    v->data = ctx->loc_conf->proxy_pass.data;
    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;

    return NGX_OK;
}


static void *ngx_stream_modbus_proxy_create_srv_conf(ngx_conf_t *cf)
{
    ngx_stream_modbus_proxy_srv_conf_t *mgcf;

    mgcf = ngx_pcalloc(cf->pool, sizeof(ngx_stream_modbus_proxy_srv_conf_t));
    if (mgcf == NULL)
    {
        return NULL;
    }

    mgcf->locations = NULL;
    mgcf->default_location = NULL;

    return mgcf;
}

static char *ngx_stream_modbus_proxy_merge_srv_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_stream_modbus_proxy_srv_conf_t *prev = parent;
    ngx_stream_modbus_proxy_srv_conf_t *conf = child;

    if (conf->locations == NULL)
    {
        conf->locations = prev->locations;
        conf->default_location = prev->default_location;
    }

    return NGX_CONF_OK;
}

static ngx_int_t ngx_stream_modbus_proxy_postconfiguration(ngx_conf_t *cf)
{
    ngx_stream_variable_t       *var;
    ngx_stream_handler_pt       *h;
    ngx_stream_core_main_conf_t *cmcf;
    ngx_str_t                    name = ngx_string("proxy_pass");
    // Register $proxy_pass so it can be used in proxy_pass.
    var = ngx_stream_add_variable(cf, &name, 0);
    if (var == NULL) {
        return NGX_ERROR;
    }

    var->get_handler = ngx_stream_modbus_proxy_variable;

    // Run our backend-selection logic in the preread phase, before the
    // proxy module evaluates $proxy_pass in the content phase.
    cmcf = ngx_stream_conf_get_module_main_conf(cf, ngx_stream_core_module);
    if (cmcf == NULL) {
        return NGX_ERROR;
    }

    h = ngx_array_push(&cmcf->phases[NGX_STREAM_PREREAD_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_stream_modbus_proxy_preread;

    ngx_log_debug0(NGX_LOG_WARN, cf->log, 0,
                       "modbus_proxy: $proxy_pass variable and preread "
                       "handler registered");

    return NGX_OK;
}
