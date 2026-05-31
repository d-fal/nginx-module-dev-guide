
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_stream.h>

static ngx_int_t ngx_stream_echo_handler(ngx_stream_session_t *s);
static ngx_int_t ngx_stream_echo_init(ngx_conf_t *cf);

static char *ngx_stream_echo_preread_merge_srv_conf(ngx_conf_t *cf, void *parent, void *child);
static void *ngx_stream_echo_preread_create_srv_conf(ngx_conf_t *cf);

typedef struct
{
    ngx_flag_t enabled;
} ngx_stream_echo_preread_srv_conf_t;

static ngx_command_t ngx_stream_echo_commands[] = {

    {ngx_string("stream_echo"),                                  /* how we activate this module in config file */
     NGX_STREAM_MAIN_CONF | NGX_STREAM_SRV_CONF | NGX_CONF_FLAG, /* module accepts no values */
     ngx_conf_set_flag_slot,                                     /* module configuration function */
     NGX_STREAM_SRV_CONF_OFFSET,
     0,
     NULL},
    ngx_null_command /* end of commands */
};

static ngx_stream_module_t ngx_stream_echo_module_ctx = {
    NULL,                 /* preconfiguration */
    ngx_stream_echo_init, /* postconfiguration */

    NULL, /* create main configuration */
    NULL, /* init main configuration */

    ngx_stream_echo_preread_create_srv_conf, /* create server configuration */
    ngx_stream_echo_preread_merge_srv_conf   /* merge server configuration */
};

ngx_module_t ngx_stream_echo_module = {
    NGX_MODULE_V1,
    &ngx_stream_echo_module_ctx, /* module context */
    ngx_stream_echo_commands,    /*module directives*/
    NGX_STREAM_MODULE,           /* module type */
    NULL,                        /* init master */
    NULL,                        /* init module */
    NULL,                        /* init process */
    NULL,                        /* init thread */
    NULL,                        /* exit thread */
    NULL,                        /* exit process */
    NULL,                        /* exit master */
    NGX_MODULE_V1_PADDING};

static ngx_int_t ngx_stream_echo_handler(ngx_stream_session_t *s)
{
    ngx_connection_t *c;
    ngx_buf_t *b;

    c = s->connection;

    ngx_log_debug0(NGX_LOG_DEBUG_STREAM, c->log, 0, "############ foo preread");
    
    ngx_str_t str;
    ngx_str_set(&str, "hello world!");

    b = ngx_create_temp_buf(c->pool, sizeof("hello\n") - 1);

    if (b == NULL)
    {
        return NGX_ERROR;
    }

    b->last = ngx_cpymem(b->last, str.data,str.len);
    ssize_t n = c->send(c, b->pos, str.len);

    ngx_log_debug1(NGX_LOG_DEBUG_STREAM, c->log, 0, "sent=%z", n);

    c->close = 1;

    return NGX_DONE;
}

static ngx_int_t ngx_stream_echo_init(ngx_conf_t *cf)
{
    ngx_stream_core_main_conf_t *cmcf;

    cmcf = ngx_stream_conf_get_module_main_conf(
        cf,
        ngx_stream_core_module);

    ngx_stream_handler_pt *h;

    h = ngx_array_push(
        &cmcf->phases[NGX_STREAM_PREREAD_PHASE].handlers);

    *h = ngx_stream_echo_handler;

    return NGX_OK;
}

static void *ngx_stream_echo_preread_create_srv_conf(ngx_conf_t *cf)
{
    ngx_stream_echo_preread_srv_conf_t *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_stream_echo_preread_srv_conf_t));
    if (conf == NULL)
    {
        return NULL;
    }

    conf->enabled = NGX_CONF_UNSET;

    return conf;
}

static char *ngx_stream_echo_preread_merge_srv_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_stream_echo_preread_srv_conf_t *prev = parent;
    ngx_stream_echo_preread_srv_conf_t *conf = child;

    ngx_conf_merge_value(conf->enabled, prev->enabled, 0);

    return NGX_CONF_OK;
}