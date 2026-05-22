
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

static ngx_int_t ngx_http_dump_body_handler(ngx_http_request_t *r);
static char *ngx_http_dump_body(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static void *ngx_http_dump_body_create_loc_conf(ngx_conf_t *cf);
static char *
ngx_http_dump_body_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);
void
ngx_http_foo_init(ngx_http_request_t *r);

typedef struct
{
} ngx_dump_body_conf_t;

static ngx_command_t ngx_http_dump_body_commands[] = {

    {ngx_string("dump_body"),           /* how we activate this module in config file */
     NGX_HTTP_LOC_CONF | NGX_CONF_NOARGS, /* echo_module accepts on/off values */
     ngx_http_dump_body,                       /* module configuration function */
     0,
     0,
     NULL},
    ngx_null_command /* end of commands */
};

static ngx_http_module_t ngx_http_dump_body_module_ctx = {
    NULL, /* preconfiguration */
    NULL, /* postconfiguration */

    NULL, /* create main configuration */
    NULL, /* init main configuration */

    NULL, /* create server configuration */
    NULL, /* merge server configuration */

    ngx_http_dump_body_create_loc_conf, /* create location configuration */
    ngx_http_dump_body_merge_loc_conf   /* merge location configuration */
};

/*
    the follwoing struct would be used by nginx to relate the code
    to the config file.
    Note that the name of this module should be the one you picked
    for $ngx_module_name.
    For this module, it is: $ngx_module_name=ngx_http_dump_body_module
*/
ngx_module_t ngx_http_dump_body_module = {
    NGX_MODULE_V1,
    &ngx_http_dump_body_module_ctx, /* module context */
    ngx_http_dump_body_commands,    /*module directives*/
    NGX_HTTP_MODULE,           /* module type */
    NULL,                      /* init master */
    NULL,                      /* init module */
    NULL,                      /* init process */
    NULL,                      /* init thread */
    NULL,                      /* exit thread */
    NULL,                      /* exit process */
    NULL,                      /* exit master */
    NGX_MODULE_V1_PADDING};

static ngx_int_t ngx_http_dump_body_handler(ngx_http_request_t *r)
{
    ngx_buf_t *b;
    ngx_chain_t out;
    ngx_str_t msg = ngx_string("echo_module not enabled!");
    // ngx_dump_body_conf_t *slcf = ngx_http_get_module_loc_conf(r, ngx_http_dump_body_module);
    // ngx_list_part_t *part = &r->headers_in.headers.part;
    ngx_int_t rc;

    /* read request body */
    rc = ngx_http_read_client_request_body(r, ngx_http_foo_init);
    if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
    /* error */
        return rc;
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

void ngx_http_foo_init(ngx_http_request_t *r)
{
    off_t         len;
    ngx_buf_t    *b;
    ngx_int_t     rc;
    ngx_chain_t  *in, out;
    ngx_str_t body;
 


    if (r->request_body == NULL) {
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    len = 0;

    for (in = r->request_body->bufs; in; in = in->next) {
         body.data = in->buf->pos;
         body.len = in->buf->last - in->buf->pos;
         len += ngx_buf_size(in->buf);
         ngx_log_debug2(NGX_LOG_DEBUG_HTTP,
                           r->connection->log, 0,
                           "http echo handler! LEN: %O <>  %V",
                           len,&body);
    }
    len++;
    b = ngx_create_temp_buf(r->pool, body.len);
    if (b == NULL) {
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }
    

    b->last = ngx_sprintf(b->pos, "%V\n",&body);
    b->last_buf = (r == r->main) ? 1 : 0;
    b->last_in_chain = 1;

    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = len;

    rc = ngx_http_send_header(r);

    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        ngx_http_finalize_request(r, rc);
        return;
    }

    out.buf = b;
    out.next = NULL;

    rc = ngx_http_output_filter(r, &out);

    ngx_http_finalize_request(r, rc);
}

static char *ngx_http_dump_body(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t *clcf = conf; /* pointer to core location configuration */

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_dump_body_handler;

    return NGX_CONF_OK;
}

static void *ngx_http_dump_body_create_loc_conf(ngx_conf_t *cf)
{
    ngx_dump_body_conf_t *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_dump_body_conf_t));
    if (conf == NULL)
    {
        return NULL;
    }
    return conf;
}

static char *
ngx_http_dump_body_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    // ngx_dump_body_conf_t *prev = parent;
    // ngx_dump_body_conf_t *conf = child;
   
    return NGX_CONF_OK;
}
