
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_stream.h>

// The Modbus RTU (serial) bridge depends on libmodbus and is compiled in only
// when NGX_STREAM_MODBUS_RTU is defined by the addon `config` (default on;
// disable with MODBUS_RTU=0 at configure time). Without it the module is a
// pure Modbus TCP router with no libmodbus header or linked library.
#if (NGX_STREAM_MODBUS_RTU)
#include <modbus/modbus.h>

// Modbus ADU/PDU limits (see modbus.h: MODBUS_MAX_ADU_LENGTH == 260).
#define NGX_MODBUS_ADU_MAX MODBUS_MAX_ADU_LENGTH
#endif

#define NGX_MODBUS_MBAP_LEN 7

// Modbus exception codes returned to the client on the rtu path:
//   0x0A "gateway path unavailable"        -> unit id has no rtu backend
//   0x0B "gateway target failed to respond"-> serial open/send/reply failed
#define NGX_MODBUS_EXC_GATEWAY_PATH 0x0A
#define NGX_MODBUS_EXC_GATEWAY_TARGET 0x0B

// deny_ops: each token maps to a single bit, keyed by the modbus function code,
// so the runtime check is a direct `deny_ops & (1UL << req[7])`. Every value must
// be a distinct bit because ngx_conf_set_bitmask_slot ORs mask.mask into the field.
// Function codes fit well within ngx_uint_t (64-bit), so 1UL << fc is safe.
#define NGX_MODBUS_OP(fc) (1UL << (fc))

static ngx_conf_bitmask_t ngx_stream_modbus_deny_ops_masks[] = {
    { ngx_string("read_coils"),     NGX_MODBUS_OP(0x01) },
    // read discrete input
    { ngx_string("read_discrete"),  NGX_MODBUS_OP(0x02) },
    // read holding register
    { ngx_string("read_holding"),   NGX_MODBUS_OP(0x03) },
    // read input register
    { ngx_string("read_input_reg"),     NGX_MODBUS_OP(0x04) },
    { ngx_string("write_coil"),     NGX_MODBUS_OP(0x05) },
    { ngx_string("write_reg"), NGX_MODBUS_OP(0x06) },
    { ngx_string("write_multiple_bits"), NGX_MODBUS_OP(0x0F) },
    { ngx_string("write_multiple_regs"), NGX_MODBUS_OP(0x10) },
    {ngx_null_string, 0}};

typedef struct ngx_stream_modbus_proxy_srv_conf_s ngx_stream_modbus_proxy_srv_conf_t;
typedef struct ngx_stream_modbus_proxy_loc_conf_s ngx_stream_modbus_proxy_loc_conf_t;

static char *ngx_stream_modbus_proxy_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_stream_modbus_proxy_preread(ngx_stream_session_t *s);
static void ngx_stream_modbus_proxy_timeout_handler(ngx_event_t *ev);
static void ngx_stream_modbus_proxy_cleanup_timer(void *data);
static ngx_int_t ngx_stream_modbus_proxy_variable(ngx_stream_session_t *s,
                                                  ngx_stream_variable_value_t *v,
                                                  uintptr_t data);
static void *ngx_stream_modbus_proxy_create_srv_conf(ngx_conf_t *cf);
static char *ngx_stream_modbus_proxy_merge_srv_conf(ngx_conf_t *cf, void *parent, void *child);
static ngx_int_t ngx_stream_modbus_proxy_postconfiguration(ngx_conf_t *cf);

// --- Modbus RTU (serial) bridge ---------------------------------------------
#if (NGX_STREAM_MODBUS_RTU)
static ngx_int_t ngx_stream_modbus_rtu_takeover(ngx_stream_session_t *s,
                                                ngx_connection_t *c, ngx_stream_modbus_proxy_loc_conf_t *loc);
static void ngx_stream_modbus_client_read(ngx_event_t *rev);
static void ngx_stream_modbus_client_write(ngx_event_t *wev);
static void ngx_stream_modbus_client_send(ngx_stream_session_t *s);
static void ngx_stream_modbus_rtu_transaction(ngx_stream_session_t *s);
static modbus_t *ngx_stream_modbus_rtu_open(ngx_stream_modbus_proxy_srv_conf_t *mgcf,
                                            ngx_log_t *log);
#endif

// Location conf (one per `modbus <id> { ... }` block)
struct ngx_stream_modbus_proxy_loc_conf_s
{
    ngx_uint_t slave_id;
    ngx_str_t proxy_pass;
    ngx_msec_t timeout; // absolute max session duration, 0 = unlimited
    ngx_str_t mode;     // modbus mode is either tcp or rtu
    ngx_uint_t deny_ops; // bitmask of denied modbus function codes (0 = none)
};

// server conf
//
// The RTU serial line is a property of the bus, not of any single slave: one
// physical RS-485 line carries many slave addresses. So the line parameters and
// the libmodbus context live here (server level), shared by every `mode rtu`
// block; each block only marks its unit id as living on that bus.
struct ngx_stream_modbus_proxy_srv_conf_s
{
    ngx_array_t *blocks;
    ngx_stream_modbus_proxy_loc_conf_t *default_location;

    // --- RTU serial bus parameters (set at server level) ----------------
    ngx_str_t serial;        // device path, e.g. /dev/ttyUSB0 (required if any rtu block)
    ngx_uint_t baud;         // 9600, 19200, ... (default 9600)
    ngx_uint_t data_bits;    // 7 or 8 (default 8)
    ngx_str_t parity;        // none | even | odd (default none)
    ngx_uint_t stop_bits;    // 1 or 2 (default 1)
    ngx_msec_t resp_timeout; // RTU reply timeout (default 1000 ms)

#if (NGX_STREAM_MODBUS_RTU)
    // --- runtime state, worker-local ------------------------------------
    // libmodbus context for the bus, opened lazily on the first rtu request and
    // kept open for the worker's lifetime (reopened after a serial error).
    modbus_t *mb;
#endif
};

// Per-session ctx. On the tcp path only `loc_conf` is used (read by the
// $proxy_pass variable handler). The remaining fields drive the rtu bridge.
typedef struct
{
    ngx_stream_modbus_proxy_loc_conf_t *loc_conf;

#if (NGX_STREAM_MODBUS_RTU)
    ngx_stream_session_t *s;
    ngx_connection_t *client;

    u_char req[NGX_MODBUS_ADU_MAX]; // inbound MBAP request accumulator
    size_t req_len;
    size_t req_need; // full frame length once MBAP known

    u_char rsp[NGX_MODBUS_ADU_MAX]; // outbound MBAP reply
    size_t rsp_len;
    size_t rsp_sent;
#endif
} ngx_stream_modbus_proxy_ctx_t;

static ngx_command_t ngx_stream_modbus_proxy_commands[] = {
    // modbus <slave_id> <mode> { ... }  -- mode (tcp|rtu) is a required 2nd
    // positional arg; the block body holds host/timeout.
    {ngx_string("modbus"),
     NGX_STREAM_SRV_CONF | NGX_CONF_BLOCK | NGX_CONF_TAKE2,
     ngx_stream_modbus_proxy_block,
     NGX_STREAM_SRV_CONF_OFFSET,
     0,
     NULL},
    {ngx_string("host"),
     NGX_STREAM_SRV_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_str_slot,
     NGX_STREAM_SRV_CONF_OFFSET,
     offsetof(ngx_stream_modbus_proxy_loc_conf_t, proxy_pass),
     NULL},
    {ngx_string("timeout"),
     NGX_STREAM_SRV_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_msec_slot,
     NGX_STREAM_SRV_CONF_OFFSET,
     offsetof(ngx_stream_modbus_proxy_loc_conf_t, timeout),
     NULL},
    {ngx_string("deny_ops"),
     NGX_STREAM_SRV_CONF | NGX_CONF_1MORE,
     ngx_conf_set_bitmask_slot,
     NGX_STREAM_SRV_CONF_OFFSET,
     offsetof(ngx_stream_modbus_proxy_loc_conf_t, deny_ops),
     &ngx_stream_modbus_deny_ops_masks},

    // RTU serial-bus parameters. These describe the shared serial line and so
    // are set at the `server` level, OUTSIDE the modbus blocks. The request's
    // unit id is used as the RTU slave address on the wire.
    {ngx_string("serial"),
     NGX_STREAM_SRV_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_str_slot,
     NGX_STREAM_SRV_CONF_OFFSET,
     offsetof(ngx_stream_modbus_proxy_srv_conf_t, serial),
     NULL},
    {ngx_string("baud"),
     NGX_STREAM_SRV_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_num_slot,
     NGX_STREAM_SRV_CONF_OFFSET,
     offsetof(ngx_stream_modbus_proxy_srv_conf_t, baud),
     NULL},
    {ngx_string("data_bits"),
     NGX_STREAM_SRV_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_num_slot,
     NGX_STREAM_SRV_CONF_OFFSET,
     offsetof(ngx_stream_modbus_proxy_srv_conf_t, data_bits),
     NULL},
    {ngx_string("parity"),
     NGX_STREAM_SRV_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_str_slot,
     NGX_STREAM_SRV_CONF_OFFSET,
     offsetof(ngx_stream_modbus_proxy_srv_conf_t, parity),
     NULL},
    {ngx_string("stop_bits"),
     NGX_STREAM_SRV_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_num_slot,
     NGX_STREAM_SRV_CONF_OFFSET,
     offsetof(ngx_stream_modbus_proxy_srv_conf_t, stop_bits),
     NULL},
    {ngx_string("response_timeout"),
     NGX_STREAM_SRV_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_msec_slot,
     NGX_STREAM_SRV_CONF_OFFSET,
     offsetof(ngx_stream_modbus_proxy_srv_conf_t, resp_timeout),
     NULL},
    ngx_null_command};

static ngx_stream_module_t ngx_stream_modbus_proxy_module_ctx = {
    NULL,                                      /* preconfiguration */
    ngx_stream_modbus_proxy_postconfiguration, /* postconfiguration */
    NULL,                                      /* create main configuration */
    NULL,                                      /* merge main configuration */
    ngx_stream_modbus_proxy_create_srv_conf,   /* create server configuration */
    ngx_stream_modbus_proxy_merge_srv_conf     /* merge server configuration */
};
ngx_module_t ngx_stream_modbus_proxy_module = {
    NGX_MODULE_V1,
    &ngx_stream_modbus_proxy_module_ctx, /* module context */
    ngx_stream_modbus_proxy_commands,    /*module directives*/
    NGX_STREAM_MODULE,                   /* module type */
    NULL,                                /* init master */
    NULL,                                /* init module */
    NULL,                                /* init process */
    NULL,                                /* init thread */
    NULL,                                /* exit thread */
    NULL,                                /* exit process */
    NULL,                                /* exit master */
    NGX_MODULE_V1_PADDING};

// directive

static ngx_stream_modbus_proxy_loc_conf_t *ngx_stream_modbus_proxy_find_location(ngx_stream_modbus_proxy_srv_conf_t *mgcf, ngx_uint_t slave_id);


// Fires when a session outlives its backend's configured timeout: close it.
static void ngx_stream_modbus_proxy_timeout_handler(ngx_event_t *ev)
{
    ngx_stream_session_t *s = ev->data;
    ngx_connection_t *c = s->connection;
    ngx_stream_upstream_t *u = s->upstream;

    ngx_log_debug0(NGX_LOG_INFO, c->log, 0,
                   "modbus_proxy: session timeout reached, closing connection");

    if (u != NULL && u->peer.connection != NULL)
    {
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

    if (ev->timer_set)
    {
        ngx_del_timer(ev);
    }
}

// Handler for modbus block: modbus <slave_id> <mode> { ... }
static char *ngx_stream_modbus_proxy_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_stream_modbus_proxy_srv_conf_t *mgcf = conf;
    ngx_str_t *value = cf->args->elts;
    ngx_stream_modbus_proxy_loc_conf_t *loc_conf;
    ngx_stream_conf_ctx_t *ctx, *pctx;
    char *rv;

    pctx = cf->ctx;
    ctx = ngx_pcalloc(cf->pool, sizeof(ngx_stream_conf_ctx_t));
    if (ctx == NULL)
    {
        return NGX_CONF_ERROR;
    }

    ctx->main_conf = pctx->main_conf;
    ctx->srv_conf = ngx_pcalloc(cf->pool, sizeof(void *) * ngx_stream_max_module);
    if (ctx->srv_conf == NULL)
    {
        return NGX_CONF_ERROR;
    }

    // Create new location configuration
    loc_conf = ngx_pcalloc(cf->pool, sizeof(ngx_stream_modbus_proxy_loc_conf_t));
    if (loc_conf == NULL)
    {
        return NGX_CONF_ERROR;
    }

    // loc_conf is zero-filled by ngx_pcalloc, so proxy_pass is already {0, NULL}.
    loc_conf->timeout = NGX_CONF_UNSET_MSEC;

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

    // Parse the required mode positional arg: modbus <slave_id> <tcp|rtu>.
    // NGX_CONF_TAKE2 guarantees value[2] is present.
    if (ngx_strcmp(value[2].data, "tcp") != 0 && ngx_strcmp(value[2].data, "rtu") != 0)
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid modbus mode \"%V\": must be tcp or rtu",
                           &value[2]);
        return NGX_CONF_ERROR;
    }
#if !(NGX_STREAM_MODBUS_RTU)
    // The serial bridge was compiled out (built without libmodbus). Reject
    // "mode rtu" at config time rather than failing mysteriously at runtime.
    if (ngx_strcmp(value[2].data, "rtu") == 0)
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "modbus mode \"rtu\" is not supported: this module "
                           "was built without libmodbus (MODBUS_RTU=0)");
        return NGX_CONF_ERROR;
    }
#endif
    loc_conf->mode = value[2];

    // deny_ops is a bitmask: 0 means "nothing denied". ngx_conf_set_bitmask_slot
    // ORs each token's bit into it, so it must start at 0 (pcalloc already does
    // this; set explicitly for clarity). Do NOT use NGX_CONF_UNSET_PTR here — that
    // is -1 (all bits set), which would deny every function code.
    loc_conf->deny_ops = 0;

    if (mgcf->blocks == NULL)
    {
        mgcf->blocks = ngx_array_create(cf->pool, 2, sizeof(ngx_stream_modbus_proxy_loc_conf_t));
        if (mgcf->blocks == NULL)
        {
            return NGX_CONF_ERROR;
        }
    }

    // Build a stream conf context for the block so that sub-directives resolve
    // their `conf` to THIS block's loc_conf. We copy the surrounding srv_conf
    // array and repoint our module's slot at loc_conf; the config engine then
    // computes conf = srv_conf[ctx_index] = loc_conf, which is what generic slot
    // setters such as ngx_conf_set_str_slot (used by `host`) write into.

    ngx_memcpy(ctx->srv_conf, pctx->srv_conf, sizeof(void *) * ngx_stream_max_module);
    ctx->srv_conf[ngx_stream_modbus_proxy_module.ctx_index] = loc_conf;

    // Save current context and set new one
    void *save = cf->ctx;
    cf->ctx = ctx;

    // Parse the block (host/timeout fill in loc_conf via their `conf` arg)
    rv = ngx_conf_parse(cf, NULL);

    // Restore context
    cf->ctx = save;

    if (rv != NGX_CONF_OK)
    {
        return rv;
    }

    if (loc_conf->timeout == NGX_CONF_UNSET_MSEC)
    {
        loc_conf->timeout = 0;
    }

    // tcp mode: "host" is required (it feeds $proxy_pass). rtu blocks carry no
    // per-block serial parameters; those live at server level and are validated
    // in merge_srv_conf once all blocks are known.
    if (ngx_strcmp(loc_conf->mode.data, "rtu") != 0 && loc_conf->proxy_pass.len == 0)
    {
        return "modbus tcp block requires a \"host\" directive";
    }

    // Store in array for later lookup, now that the block has been parsed and
    // proxy_pass is populated.
    ngx_stream_modbus_proxy_loc_conf_t *new_loc = ngx_array_push(mgcf->blocks);
    if (new_loc == NULL)
    {
        return NGX_CONF_ERROR;
    }
    *new_loc = *loc_conf;

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
    ngx_stream_modbus_proxy_loc_conf_t *blocks;

    if (mgcf->blocks == NULL)
    {
        return mgcf->default_location;
    }

    blocks = mgcf->blocks->elts;

    // Exact match
    for (i = 0; i < mgcf->blocks->nelts; i++)
    {
        if (blocks[i].slave_id == slave_id)
        {
            return &blocks[i];
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
    ngx_connection_t *c = s->connection;
    ngx_stream_modbus_proxy_srv_conf_t *mgcf;
    ngx_stream_modbus_proxy_loc_conf_t *loc;
    ngx_stream_modbus_proxy_ctx_t *ctx;
    ngx_uint_t slave_id;
    ngx_uint_t op_code;

    ctx = ngx_stream_get_module_ctx(s, ngx_stream_modbus_proxy_module);
    if (ctx != NULL)
    {
        // Backend already chosen on an earlier preread invocation.
        return NGX_OK;
    }

    mgcf = ngx_stream_get_module_srv_conf(s, ngx_stream_modbus_proxy_module);
    if (mgcf == NULL)
    {
        return NGX_OK;
    }

    // Wait until the 7-byte MBAP header is fully buffered.
    if (c->buffer == NULL || (size_t)(c->buffer->last - c->buffer->pos) < 8)
    {
        return NGX_AGAIN;
    }

    op_code = c->buffer->pos[7];
    slave_id = c->buffer->pos[6];

    loc = ngx_stream_modbus_proxy_find_location(mgcf, slave_id);
    if (loc == NULL)
    {
        ngx_log_error(NGX_LOG_ERR, c->log, 0,
                      "modbus_proxy: no backend for slave_id=%ui", slave_id);
        return NGX_STREAM_BAD_GATEWAY;
    }

    // Reject denied function codes before forwarding (applies to both tcp and
    // rtu modes). deny_ops is a bitmask keyed by function code; strip the high
    // exception bit defensively. NGX_STREAM_FORBIDDEN (403) closes the session.
    if (loc->deny_ops & NGX_MODBUS_OP(op_code & 0x7f))
    {
        ngx_log_error(NGX_LOG_ERR, c->log, 0,
                      "modbus_proxy: denied op 0x%02Xi for slave_id=%ui",
                      op_code, slave_id);
        return NGX_STREAM_FORBIDDEN;
    }

    // "mode rtu": bridge to a serial port ourselves. We take over the client
    // connection and return NGX_DONE so the TCP proxy content handler is never
    // reached (see ngx_stream_core_preread_phase). The tcp path below is
    // unchanged and still returns NGX_OK.
#if (NGX_STREAM_MODBUS_RTU)
    if (ngx_strcmp(loc->mode.data, "rtu") == 0)
    {
        return ngx_stream_modbus_rtu_takeover(s, c, loc);
    }
#endif

    // tcp mode: a backend host is required; the proxy module forwards via $proxy_pass.
    if (loc->proxy_pass.len == 0)
    {
        ngx_log_error(NGX_LOG_ERR, c->log, 0,
                      "modbus_proxy: no backend for slave_id=%ui", slave_id);
        return NGX_STREAM_BAD_GATEWAY;
    }

    ctx = ngx_pcalloc(c->pool, sizeof(ngx_stream_modbus_proxy_ctx_t));
    if (ctx == NULL)
    {
        return NGX_ERROR;
    }

    ctx->loc_conf = loc;
    ngx_stream_set_ctx(s, ctx, ngx_stream_modbus_proxy_module);

    ngx_log_debug3(NGX_LOG_DEBUG_STREAM, c->log, 0,
                   "##### modbus_proxy: slave_id=%ui -> %V, func code: %X",
                   slave_id, &loc->proxy_pass, op_code);
    ngx_log_debug1(NGX_LOG_DEBUG_STREAM, c->log, 0,
                   "#### modbus_proxy: mode=%V",
                   &loc->mode);

    // Arm the absolute session-duration timer for this backend, if configured.
    if (loc->timeout)
    {
        ngx_event_t *tev;
        ngx_pool_cleanup_t *cln;

        tev = ngx_pcalloc(c->pool, sizeof(ngx_event_t));
        if (tev == NULL)
        {
            return NGX_ERROR;
        }

        tev->handler = ngx_stream_modbus_proxy_timeout_handler;
        tev->data = s;
        tev->log = c->log;

        // Ensure the timer is cancelled if the session closes on its own first.
        cln = ngx_pool_cleanup_add(c->pool, 0);
        if (cln == NULL)
        {
            return NGX_ERROR;
        }
        cln->handler = ngx_stream_modbus_proxy_cleanup_timer;
        cln->data = tev;

        ngx_add_timer(tev, loc->timeout);
    }

    return NGX_OK;
}

#if (NGX_STREAM_MODBUS_RTU)
// ============================================================================
// Modbus RTU (serial) bridge  --  "mode rtu"
//
// When a unit id maps to an rtu block we don't proxy TCP; we take over the
// client connection in the preread phase and bridge each Modbus/TCP request to
// a serial line using libmodbus. The client side stays on nginx's event loop
// (non-blocking); the serial round-trip is a single *blocking* libmodbus call
// (modbus_send_raw_request + modbus_receive_confirmation), which stalls the
// worker for up to response_timeout. That is acceptable for this low-traffic
// gateway (master_process off). The flow per request is:
//
//   client_read (accumulate full MBAP frame)
//     -> rtu_transaction (route by unit id, blocking libmodbus round-trip)
//     -> client_send (write MBAP reply) -> wait for the next request
// ============================================================================

// Lazily open and configure the libmodbus RTU context for the serial bus. Kept
// open for the worker's lifetime; returns NULL on failure (logged).
static modbus_t *
ngx_stream_modbus_rtu_open(ngx_stream_modbus_proxy_srv_conf_t *mgcf, ngx_log_t *log)
{
    modbus_t *mb;
    char parity;

    parity = (mgcf->parity.data[0] == 'e')   ? 'E'
             : (mgcf->parity.data[0] == 'o') ? 'O'
                                             : 'N';

    mb = modbus_new_rtu((const char *)mgcf->serial.data, (int)mgcf->baud,
                        parity, (int)mgcf->data_bits, (int)mgcf->stop_bits);
    if (mb == NULL)
    {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "modbus_proxy: modbus_new_rtu(\"%V\") failed: %s",
                      &mgcf->serial, modbus_strerror(errno));
        return NULL;
    }

    // Recover the link on serial errors and flush the line on a bad CRC.
    modbus_set_error_recovery(mb,
                              MODBUS_ERROR_RECOVERY_LINK | MODBUS_ERROR_RECOVERY_PROTOCOL);
    modbus_set_response_timeout(mb, mgcf->resp_timeout / 1000,
                                (mgcf->resp_timeout % 1000) * 1000);

    if (modbus_connect(mb) == -1)
    {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "modbus_proxy: modbus_connect(\"%V\") failed: %s",
                      &mgcf->serial, modbus_strerror(errno));
        modbus_free(mb);
        return NULL;
    }

    ngx_log_error(NGX_LOG_INFO, log, 0,
                  "modbus_proxy: opened serial %V @ %ui baud", &mgcf->serial, mgcf->baud);
    return mb;
}

// Build a Modbus exception reply (fc|0x80, exc) into the ctx response buffer.
static void
ngx_stream_modbus_build_exception(ngx_stream_modbus_proxy_ctx_t *rctx, u_char exc)
{
    rctx->rsp[0] = rctx->req[0]; // transaction id (echoed)
    rctx->rsp[1] = rctx->req[1];
    rctx->rsp[2] = 0; // protocol id
    rctx->rsp[3] = 0;
    rctx->rsp[4] = 0; // length = unit + fc + exc = 3
    rctx->rsp[5] = 3;
    rctx->rsp[6] = rctx->req[6];        // unit id
    rctx->rsp[7] = rctx->req[7] | 0x80; // function code with exception bit
    rctx->rsp[8] = exc;
    rctx->rsp_len = 9;
    rctx->rsp_sent = 0;
}

// Bridge one buffered MBAP request to the serial line and build the reply.
// Always leaves a frame in rctx->rsp (a real reply or a Modbus exception) and
// hands off to client_send; never finalizes the session itself, so the
// persistent connection survives per-request failures.
static void
ngx_stream_modbus_rtu_transaction(ngx_stream_session_t *s)
{
    ngx_connection_t *c = s->connection;
    ngx_stream_modbus_proxy_ctx_t *rctx;
    ngx_stream_modbus_proxy_srv_conf_t *mgcf;
    ngx_stream_modbus_proxy_loc_conf_t *loc;
    u_char raw[NGX_MODBUS_ADU_MAX];
    u_char conf[NGX_MODBUS_ADU_MAX];
    ngx_uint_t unit;
    size_t pdu_len;
    int raw_len, ret;

    rctx = ngx_stream_get_module_ctx(s, ngx_stream_modbus_proxy_module);
    mgcf = ngx_stream_get_module_srv_conf(s, ngx_stream_modbus_proxy_module);

    unit = rctx->req[6];

    // Route this request by its own unit id (a persistent connection may target
    // several units). Anything that is not an rtu block -> gateway path error.
    loc = ngx_stream_modbus_proxy_find_location(mgcf, unit);
    if (loc == NULL || ngx_strcmp(loc->mode.data, "rtu") != 0)
    {
        ngx_log_error(NGX_LOG_ERR, c->log, 0,
                      "modbus_proxy: unit %ui has no rtu backend", unit);
        ngx_stream_modbus_build_exception(rctx, NGX_MODBUS_EXC_GATEWAY_PATH);
        ngx_stream_modbus_client_send(s);
        return;
    }

    if (mgcf->mb == NULL)
    {
        mgcf->mb = ngx_stream_modbus_rtu_open(mgcf, c->log);
        if (mgcf->mb == NULL)
        {
            ngx_stream_modbus_build_exception(rctx, NGX_MODBUS_EXC_GATEWAY_TARGET);
            ngx_stream_modbus_client_send(s);
            return;
        }
    }

    modbus_set_slave(mgcf->mb, (int)unit);
    modbus_flush(mgcf->mb);

    // RTU request = [unit][PDU]; libmodbus appends the CRC. The MBAP length
    // field (req[4..5]) counts the unit id + PDU, so PDU = req_need - 7.
    pdu_len = rctx->req_need - NGX_MODBUS_MBAP_LEN;
    raw[0] = (u_char)unit;
    ngx_memcpy(raw + 1, rctx->req + NGX_MODBUS_MBAP_LEN, pdu_len);
    raw_len = (int)(1 + pdu_len);

    ngx_log_debug2(NGX_LOG_DEBUG_STREAM, c->log, 0,
                   "modbus_proxy: rtu unit=%ui fc=%ud", unit, rctx->req[7]);

    if (modbus_send_raw_request(mgcf->mb, raw, raw_len) == -1)
    {
        ngx_log_error(NGX_LOG_ERR, c->log, 0,
                      "modbus_proxy: send_raw_request failed: %s", modbus_strerror(errno));
        modbus_close(mgcf->mb);
        modbus_free(mgcf->mb);
        mgcf->mb = NULL;
        ngx_stream_modbus_build_exception(rctx, NGX_MODBUS_EXC_GATEWAY_TARGET);
        ngx_stream_modbus_client_send(s);
        return;
    }

    // Blocking: waits up to response_timeout for the slave's reply.
    ret = modbus_receive_confirmation(mgcf->mb, conf);
    if (ret < 0)
    {
        ngx_log_error(NGX_LOG_ERR, c->log, 0,
                      "modbus_proxy: no/garbled RTU reply: %s", modbus_strerror(errno));
        // EMBBADCRC is a frame error, not a link error; otherwise drop the link.
        if (errno != EMBBADCRC)
        {
            modbus_close(mgcf->mb);
            modbus_free(mgcf->mb);
            mgcf->mb = NULL;
        }
        ngx_stream_modbus_build_exception(rctx, NGX_MODBUS_EXC_GATEWAY_TARGET);
        ngx_stream_modbus_client_send(s);
        return;
    }

    // RTU confirmation = [unit][PDU][CRC-lo][CRC-hi]; PDU = ret - 3, at conf[1].
    if (ret < 3)
    {
        ngx_log_error(NGX_LOG_ERR, c->log, 0,
                      "modbus_proxy: short RTU reply (%d bytes)", ret);
        ngx_stream_modbus_build_exception(rctx, NGX_MODBUS_EXC_GATEWAY_TARGET);
        ngx_stream_modbus_client_send(s);
        return;
    }
    pdu_len = (size_t)ret - 3;

    // Build the MBAP reply: echo txn id, protocol 0, length = unit + PDU.
    rctx->rsp[0] = rctx->req[0];
    rctx->rsp[1] = rctx->req[1];
    rctx->rsp[2] = 0;
    rctx->rsp[3] = 0;
    rctx->rsp[4] = (u_char)((1 + pdu_len) >> 8);
    rctx->rsp[5] = (u_char)((1 + pdu_len) & 0xff);
    rctx->rsp[6] = (u_char)unit;
    ngx_memcpy(rctx->rsp + 7, conf + 1, pdu_len);
    rctx->rsp_len = 7 + pdu_len;
    rctx->rsp_sent = 0;

    ngx_stream_modbus_client_send(s);
}

// Write the buffered MBAP reply to the client, then wait for the next request.
static void
ngx_stream_modbus_client_send(ngx_stream_session_t *s)
{
    ngx_connection_t *c = s->connection;
    ngx_stream_modbus_proxy_ctx_t *rctx;
    ssize_t n;
    size_t leftover;

    rctx = ngx_stream_get_module_ctx(s, ngx_stream_modbus_proxy_module);

    while (rctx->rsp_sent < rctx->rsp_len)
    {
        n = c->send(c, rctx->rsp + rctx->rsp_sent, rctx->rsp_len - rctx->rsp_sent);

        if (n == NGX_AGAIN)
        {
            if (ngx_handle_write_event(c->write, 0) != NGX_OK)
            {
                ngx_stream_finalize_session(s, NGX_STREAM_INTERNAL_SERVER_ERROR);
            }
            return;
        }

        if (n == NGX_ERROR)
        {
            ngx_stream_finalize_session(s, NGX_STREAM_OK);
            return;
        }

        rctx->rsp_sent += n;
    }

    // Reply delivered. Reset for the next request, carrying over any pipelined
    // bytes that arrived past the end of the frame we just served.
    leftover = (rctx->req_len > rctx->req_need) ? (rctx->req_len - rctx->req_need) : 0;
    if (leftover > 0)
    {
        ngx_memmove(rctx->req, rctx->req + rctx->req_need, leftover);
    }
    rctx->req_len = leftover;
    rctx->req_need = 0;
    rctx->rsp_len = 0;
    rctx->rsp_sent = 0;

    if (ngx_handle_read_event(c->read, 0) != NGX_OK)
    {
        ngx_stream_finalize_session(s, NGX_STREAM_INTERNAL_SERVER_ERROR);
        return;
    }

    // A pipelined request was already (partly) buffered: process it now.
    if (rctx->req_len > 0)
    {
        ngx_stream_modbus_client_read(c->read);
    }
}

// Client write handler: resume a reply that previously blocked on NGX_AGAIN.
static void
ngx_stream_modbus_client_write(ngx_event_t *wev)
{
    ngx_connection_t *c = wev->data;
    ngx_stream_session_t *s = c->data;

    if (wev->timedout)
    {
        ngx_log_error(NGX_LOG_INFO, c->log, 0, "modbus_proxy: client write timed out");
        ngx_stream_finalize_session(s, NGX_STREAM_OK);
        return;
    }

    ngx_stream_modbus_client_send(s);
}

// Client read handler: accumulate a full MBAP request, then bridge it to RTU.
static void
ngx_stream_modbus_client_read(ngx_event_t *rev)
{
    ngx_connection_t *c = rev->data;
    ngx_stream_session_t *s = c->data;
    ngx_stream_modbus_proxy_ctx_t *rctx;
    ssize_t n;
    size_t want;

    rctx = ngx_stream_get_module_ctx(s, ngx_stream_modbus_proxy_module);

    if (rev->timedout)
    {
        ngx_log_error(NGX_LOG_INFO, c->log, 0, "modbus_proxy: client read timed out");
        ngx_stream_finalize_session(s, NGX_STREAM_OK);
        return;
    }

    for (;;)
    {

        if (rctx->req_len < NGX_MODBUS_MBAP_LEN)
        {
            want = NGX_MODBUS_MBAP_LEN - rctx->req_len;
        }
        else
        {
            // MBAP length field (bytes 4..5) counts the unit id + PDU bytes.
            rctx->req_need = 6 + ((rctx->req[4] << 8) | rctx->req[5]);

            if (rctx->req_need < NGX_MODBUS_MBAP_LEN || rctx->req_need > NGX_MODBUS_ADU_MAX)
            {
                ngx_log_error(NGX_LOG_ERR, c->log, 0,
                              "modbus_proxy: bad MBAP length %uz", rctx->req_need);
                ngx_stream_finalize_session(s, NGX_STREAM_BAD_REQUEST);
                return;
            }

            if (rctx->req_len >= rctx->req_need)
            {
                break; // full frame buffered
            }

            want = rctx->req_need - rctx->req_len;
        }

        n = c->recv(c, rctx->req + rctx->req_len, want);

        if (n == NGX_AGAIN)
        {
            if (ngx_handle_read_event(rev, 0) != NGX_OK)
            {
                ngx_stream_finalize_session(s, NGX_STREAM_INTERNAL_SERVER_ERROR);
            }
            return;
        }

        if (n == NGX_ERROR || n == 0)
        {
            ngx_stream_finalize_session(s, NGX_STREAM_OK);
            return;
        }

        rctx->req_len += n;
    }

    if (rctx->req[2] != 0 || rctx->req[3] != 0)
    {
        ngx_log_error(NGX_LOG_ERR, c->log, 0, "modbus_proxy: non-zero protocol id");
        ngx_stream_finalize_session(s, NGX_STREAM_BAD_REQUEST);
        return;
    }

    ngx_stream_modbus_rtu_transaction(s);
}

// Take over the client connection for an rtu unit and start the bridge loop.
// Returns NGX_DONE so the preread phase stops without running the TCP proxy.
static ngx_int_t
ngx_stream_modbus_rtu_takeover(ngx_stream_session_t *s, ngx_connection_t *c,
                               ngx_stream_modbus_proxy_loc_conf_t *loc)
{
    ngx_stream_modbus_proxy_ctx_t *rctx;
    size_t buffered;

    rctx = ngx_pcalloc(c->pool, sizeof(ngx_stream_modbus_proxy_ctx_t));
    if (rctx == NULL)
    {
        ngx_stream_finalize_session(s, NGX_STREAM_INTERNAL_SERVER_ERROR);
        return NGX_DONE;
    }

    rctx->loc_conf = loc;
    rctx->s = s;
    rctx->client = c;
    ngx_stream_set_ctx(s, rctx, ngx_stream_modbus_proxy_module);

    // Seed the request accumulator with bytes nginx already prebuffered, then
    // mark the preread buffer consumed.
    buffered = c->buffer->last - c->buffer->pos;
    if (buffered > NGX_MODBUS_ADU_MAX)
    {
        buffered = NGX_MODBUS_ADU_MAX;
    }
    ngx_memcpy(rctx->req, c->buffer->pos, buffered);
    rctx->req_len = buffered;
    c->buffer->pos = c->buffer->last;

    c->read->handler = ngx_stream_modbus_client_read;
    c->write->handler = ngx_stream_modbus_client_write;

    // Optional absolute session-duration cap (reuses the tcp-path handler,
    // which finalizes directly when s->upstream is NULL, as it is for rtu).
    if (loc->timeout)
    {
        ngx_event_t *tev;
        ngx_pool_cleanup_t *cln;

        tev = ngx_pcalloc(c->pool, sizeof(ngx_event_t));
        if (tev == NULL)
        {
            ngx_stream_finalize_session(s, NGX_STREAM_INTERNAL_SERVER_ERROR);
            return NGX_DONE;
        }
        tev->handler = ngx_stream_modbus_proxy_timeout_handler;
        tev->data = s;
        tev->log = c->log;

        cln = ngx_pool_cleanup_add(c->pool, 0);
        if (cln == NULL)
        {
            ngx_stream_finalize_session(s, NGX_STREAM_INTERNAL_SERVER_ERROR);
            return NGX_DONE;
        }
        cln->handler = ngx_stream_modbus_proxy_cleanup_timer;
        cln->data = tev;

        ngx_add_timer(tev, loc->timeout);
    }

    ngx_log_debug1(NGX_LOG_DEBUG_STREAM, c->log, 0,
                   "modbus_proxy: rtu takeover slave_id=%ui", loc->slave_id);

    if (ngx_handle_read_event(c->read, 0) != NGX_OK)
    {
        ngx_stream_finalize_session(s, NGX_STREAM_INTERNAL_SERVER_ERROR);
        return NGX_DONE;
    }

    // Process any bytes already buffered during preread (and wait for more).
    ngx_stream_modbus_client_read(c->read);

    return NGX_DONE;
}
#endif // NGX_STREAM_MODBUS_RTU

// $modbus_backend: returns the backend selected during the preread phase.
static ngx_int_t ngx_stream_modbus_proxy_variable(ngx_stream_session_t *s,
                                                  ngx_stream_variable_value_t *v,
                                                  uintptr_t data)
{
    ngx_stream_modbus_proxy_ctx_t *ctx;

    ctx = ngx_stream_get_module_ctx(s, ngx_stream_modbus_proxy_module);

    if (ctx == NULL || ctx->loc_conf == NULL || ctx->loc_conf->proxy_pass.len == 0)
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

    mgcf->blocks = NULL;
    mgcf->default_location = NULL;

    // Generic setters treat 0 as "already set"; seed UNSET sentinels so the
    // server-level directives assign once and merge can apply defaults.
    mgcf->baud = NGX_CONF_UNSET_UINT;
    mgcf->data_bits = NGX_CONF_UNSET_UINT;
    mgcf->stop_bits = NGX_CONF_UNSET_UINT;
    mgcf->resp_timeout = NGX_CONF_UNSET_MSEC;
#if (NGX_STREAM_MODBUS_RTU)
    mgcf->mb = NULL;
#endif

    return mgcf;
}

static char *ngx_stream_modbus_proxy_merge_srv_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_stream_modbus_proxy_srv_conf_t *prev = parent;
    ngx_stream_modbus_proxy_srv_conf_t *conf = child;
    ngx_stream_modbus_proxy_loc_conf_t *blocks;
    ngx_uint_t i;

    if (conf->blocks == NULL)
    {
        conf->blocks = prev->blocks;
        conf->default_location = prev->default_location;
    }

    // RTU serial-bus parameters: inherit from the surrounding scope, then default.
    ngx_conf_merge_str_value(conf->serial, prev->serial, "");
    ngx_conf_merge_uint_value(conf->baud, prev->baud, 9600);
    ngx_conf_merge_uint_value(conf->data_bits, prev->data_bits, 8);
    ngx_conf_merge_uint_value(conf->stop_bits, prev->stop_bits, 1);
    ngx_conf_merge_str_value(conf->parity, prev->parity, "none");
    ngx_conf_merge_msec_value(conf->resp_timeout, prev->resp_timeout, 1000);

    if (conf->data_bits != 7 && conf->data_bits != 8)
    {
        return "modbus \"data_bits\" must be 7 or 8";
    }
    if (conf->stop_bits != 1 && conf->stop_bits != 2)
    {
        return "modbus \"stop_bits\" must be 1 or 2";
    }
    if (ngx_strcmp(conf->parity.data, "none") != 0 && ngx_strcmp(conf->parity.data, "even") != 0 && ngx_strcmp(conf->parity.data, "odd") != 0)
    {
        return "modbus \"parity\" must be none, even or odd";
    }

    // If any modbus block on this server is rtu, a serial device is required.
    if (conf->serial.len == 0 && conf->blocks != NULL)
    {
        blocks = conf->blocks->elts;
        for (i = 0; i < conf->blocks->nelts; i++)
        {
            if (ngx_strcmp(blocks[i].mode.data, "rtu") == 0)
            {
                return "a \"mode rtu\" modbus block requires a server-level "
                       "\"serial\" directive";
            }
        }
    }

    return NGX_CONF_OK;
}

static ngx_int_t ngx_stream_modbus_proxy_postconfiguration(ngx_conf_t *cf)
{
    ngx_stream_variable_t *var;
    ngx_stream_handler_pt *h;
    ngx_stream_core_main_conf_t *cmcf;
    ngx_str_t name = ngx_string("proxy_pass");
    // Register $proxy_pass so it can be used in proxy_pass.
    var = ngx_stream_add_variable(cf, &name, 0);
    if (var == NULL)
    {
        return NGX_ERROR;
    }

    var->get_handler = ngx_stream_modbus_proxy_variable;

    // Run our backend-selection logic in the preread phase, before the
    // proxy module evaluates $proxy_pass in the content phase.
    cmcf = ngx_stream_conf_get_module_main_conf(cf, ngx_stream_core_module);
    if (cmcf == NULL)
    {
        return NGX_ERROR;
    }

    h = ngx_array_push(&cmcf->phases[NGX_STREAM_PREREAD_PHASE].handlers);
    if (h == NULL)
    {
        return NGX_ERROR;
    }

    *h = ngx_stream_modbus_proxy_preread;

    ngx_log_debug0(NGX_LOG_WARN, cf->log, 0,
                   "modbus_proxy: $proxy_pass variable and preread "
                   "handler registered");

    return NGX_OK;
}
