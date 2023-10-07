
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

static ngx_int_t ngx_http_echo_handler(ngx_http_request_t *r);
static char *ngx_http_echo(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static void *ngx_http_echo_create_loc_conf(ngx_conf_t *cf);
static char *
ngx_http_echo_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);

typedef struct
{
    ngx_flag_t enable;
} ngx_echo_conf_t;

static ngx_command_t ngx_http_echo_commands[] = {

    {ngx_string("echo_module"),           /* how we activate this module in config file */
     NGX_HTTP_LOC_CONF | NGX_CONF_NOARGS, /* echo_module accepts on/off values */
     ngx_http_echo,                       /* module configuration function */
     0,
     0,
     NULL},
    {ngx_string("echo_enabled"),        /* how we activate this module in config file */
     NGX_HTTP_LOC_CONF | NGX_CONF_FLAG, /* echo_module accepts on/off values */
     ngx_conf_set_flag_slot,            /* module configuration function */
     NGX_HTTP_LOC_CONF_OFFSET,
     offsetof(ngx_echo_conf_t, enable),
     NULL},
    ngx_null_command /* end of commands */
};

static ngx_http_module_t ngx_http_echo_module_ctx = {
    NULL, /* preconfiguration */
    NULL, /* postconfiguration */

    NULL, /* create main configuration */
    NULL, /* init main configuration */

    NULL, /* create server configuration */
    NULL, /* merge server configuration */

    ngx_http_echo_create_loc_conf, /* create location configuration */
    ngx_http_echo_merge_loc_conf   /* merge location configuration */
};

/*
    the follwoing struct would be used by nginx to relate the code
    to the config file.
    Note that the name of this module should be the one you picked
    for $ngx_module_name.
    For this module, it is: $ngx_module_name=ngx_http_echo_module
*/
ngx_module_t ngx_http_echo_module = {
    NGX_MODULE_V1,
    &ngx_http_echo_module_ctx, /* module context */
    ngx_http_echo_commands,    /*module directives*/
    NGX_HTTP_MODULE,           /* module type */
    NULL,                      /* init master */
    NULL,                      /* init module */
    NULL,                      /* init process */
    NULL,                      /* init thread */
    NULL,                      /* exit thread */
    NULL,                      /* exit process */
    NULL,                      /* exit master */
    NGX_MODULE_V1_PADDING};

static ngx_int_t ngx_http_echo_handler(ngx_http_request_t *r)
{
    ngx_buf_t *b;
    ngx_chain_t out;
    ngx_str_t msg = ngx_string("echo_module not enabled!");
    ngx_echo_conf_t *slcf = ngx_http_get_module_loc_conf(r, ngx_http_echo_module);
    ngx_list_part_t *part = &r->headers_in.headers.part;
    ngx_table_elt_t *h = part->elts;
    if (slcf->enable)
    {
        for (size_t i = 0; i < part->nelts; i++)
        {
            if (i >= part->nelts)
            {
                if (part->next == NULL)
                {
                    break;
                }
                part = part->next;
                h = part->elts;
                i = 0;
            }

            ngx_log_debug2(NGX_LOG_DEBUG_HTTP,
                           r->connection->log, 0,
                           "http echo handler! %V : %V",
                           h[i].key, h[i].value);
        }

        ngx_str_set(&msg, "header-dump written in logs!");
    }

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "http echo handler!");

    /* Allocate a new buffer for sending out the reply. */
    b = ngx_pcalloc(r->pool, sizeof(ngx_buf_t));

    /* Insertion in the buffer chain. */
    out.buf = b;
    out.next = NULL; /* just one buffer */

    b->pos = msg.data;            /* first position in memory of the data */
    b->last = msg.data + msg.len; /* last position in memory of the data */
    b->memory = 1;                /* content is in read-only memory */
    b->last_buf = 1;              /* there will be no more buffers in the request */

    /* Sending the headers for the reply. */
    r->headers_out.status = NGX_HTTP_OK; /* 200 status code */
    /* Get the content length of the body. */
    r->headers_out.content_length_n = msg.len;
    ngx_http_send_header(r); /* Send the headers */

    /* Send the body, and return the status code of the output filter chain. */
    return ngx_http_output_filter(r, &out);
}

static char *ngx_http_echo(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t *clcf = conf; /* pointer to core location configuration */

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_echo_handler;

    return NGX_CONF_OK;
}

static void *ngx_http_echo_create_loc_conf(ngx_conf_t *cf)
{
    ngx_echo_conf_t *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_echo_conf_t));
    if (conf == NULL)
    {
        return NULL;
    }
    // conf->enable =1;
    /*
     * set by ngx_pcalloc():
     *
     *     conf->text = { 0, NULL };
     */
    conf->enable = NGX_CONF_UNSET;
    return conf;
}
static char *
ngx_http_echo_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_echo_conf_t *prev = parent;
    ngx_echo_conf_t *conf = child;

    // ngx_conf_merge_ptr_value(conf->text, prev->text, NULL);
    ngx_conf_merge_value(conf->enable, prev->enable, NGX_HTTP_OK);

    return NGX_CONF_OK;
}
