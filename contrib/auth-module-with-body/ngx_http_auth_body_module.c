
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

static void *ngx_http_auth_body_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_auth_body_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);

ngx_int_t ngx_http_subrequest_done(ngx_http_request_t *r, void *data, ngx_int_t rc);
static ngx_int_t ngx_http_auth_body_init(ngx_conf_t *cf);

typedef struct
{
    ngx_uint_t done;
    ngx_uint_t status;
    ngx_http_request_t *subrequest;
} ngx_http_body_request_ctx_t;

typedef struct
{
    ngx_flag_t enable;
    ngx_str_t uri;
} ngx_http_auth_body_loc_conf_t;

static ngx_command_t ngx_hello_body_commands[] = {

    {ngx_string("allow_body"),
     NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_flag_slot,
     NGX_HTTP_LOC_CONF_OFFSET,
     offsetof(ngx_http_auth_body_loc_conf_t, enable),
     NULL},
    {ngx_string("cauth"),
     NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
     ngx_conf_set_str_slot,
     NGX_HTTP_LOC_CONF_OFFSET,
     offsetof(ngx_http_auth_body_loc_conf_t, uri),
     NULL},
    ngx_null_command};

/* The module context. */
static ngx_http_module_t ngx_http_hello_body_module_ctx = {
    NULL,                    /* preconfiguration */
    ngx_http_auth_body_init, /* postconfiguration */

    NULL, /* create main configuration */
    NULL, /* init main configuration */

    NULL, /* create server configuration */
    NULL, /* merge server configuration */

    ngx_http_auth_body_create_loc_conf, /* create location configuration */
    ngx_http_auth_body_merge_loc_conf   /* merge location configuration */
};

/* Module definition. */
ngx_module_t ngx_http_hello_body_module = {
    NGX_MODULE_V1,
    &ngx_http_hello_body_module_ctx, /* module context */
    ngx_hello_body_commands,         /* module directives */
    NGX_HTTP_MODULE,                 /* module type */
    NULL,                            /* init master */
    NULL,                            /* init module */
    NULL,                            /* init process */
    NULL,                            /* init thread */
    NULL,                            /* exit thread */
    NULL,                            /* exit process */
    NULL,                            /* exit master */
    NGX_MODULE_V1_PADDING};

static void *ngx_http_auth_body_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_auth_body_loc_conf_t *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_auth_body_loc_conf_t));
    if (conf == NULL)
    {
        return NULL;
    }

    conf->enable = NGX_CONF_UNSET;

    return conf;
}

static char *
ngx_http_auth_body_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_auth_body_loc_conf_t *prev = parent;
    ngx_http_auth_body_loc_conf_t *conf = child;

    ngx_conf_merge_str_value(conf->uri, prev->uri, "");
    ngx_conf_merge_value(conf->enable, prev->enable, 0);

    return NGX_CONF_OK;
}

static ngx_int_t
ngx_http_auth_body_request_done(ngx_http_request_t *r, void *data, ngx_int_t rc)
{

    ngx_http_body_request_ctx_t *ctx = data;

    ctx->done = 1;
    ctx->status = r->headers_out.status;

    return rc;
}

static ngx_int_t ngx_http_auth_body_handler(ngx_http_request_t *r)
{

    ngx_http_request_t *sr;
    ngx_http_post_subrequest_t *ps;
    ngx_http_auth_body_loc_conf_t *plcf;
    ngx_http_body_request_ctx_t *ctx;
    ngx_list_t *hs;
    ngx_buf_t *b;
    ngx_chain_t out, *in;

    plcf = ngx_http_get_module_loc_conf(r, ngx_http_hello_body_module);

    ctx = ngx_http_get_module_ctx(r, ngx_http_hello_body_module);

    if (ctx != NULL)
    {
        if (!ctx->done)
        {
            return NGX_AGAIN;
        }
        if (ctx->status == NGX_HTTP_UNAUTHORIZED || ctx->status == NGX_HTTP_FORBIDDEN)
        {
            if (plcf->enable)
            {

                sr = ctx->subrequest;
                r->headers_out.content_type = sr->headers_out.content_type;

                hs = &sr->headers_out.headers;
                r->headers_out.headers = *hs;

                b = ngx_calloc_buf(r->pool);
                if (b == NULL)
                {
                    return NGX_ERROR;
                }

                r->headers_out.status = ctx->status;
                b->last_buf = 1;
                b->last_in_chain = 1;
                b->memory = 1;
                out.buf = b;
                out.next = NULL;

                in = ctx->subrequest->out;
                in->next = &out;

                ngx_http_send_header(r);
                return ngx_http_output_filter(r, in);
            }
            return ctx->status;
        }

        return NGX_OK;
    }

    ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_body_request_ctx_t));
    if (ctx == NULL)
    {
        return NGX_ERROR;
    }

    ps = ngx_palloc(r->pool, sizeof(ngx_http_post_subrequest_t));
    if (ps == NULL)
    {
        return NGX_ERROR;
    }

    ps->data = ctx;
    ps->handler = ngx_http_auth_body_request_done;

    if (ngx_http_subrequest(r, &plcf->uri, &r->args, &sr, ps, NGX_HTTP_SUBREQUEST_IN_MEMORY) != NGX_OK)
    {
        return NGX_ERROR;
    }

    ctx->subrequest = sr;
    // sr->header_only = 1;

    ngx_http_set_ctx(r, ctx, ngx_http_hello_body_module);
    return NGX_AGAIN;
}

static ngx_int_t ngx_http_auth_body_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt *h;

    ngx_http_core_main_conf_t *cmcf;
    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_ACCESS_PHASE].handlers);
    if (h == NULL)
    {
        return NGX_ERROR;
    }
    *h = ngx_http_auth_body_handler;

    return NGX_OK;
}