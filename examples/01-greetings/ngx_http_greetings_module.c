
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

static ngx_int_t ngx_http_greetings_handler(ngx_http_request_t *r);
static char *ngx_http_greetings(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);


static ngx_command_t ngx_http_greetings_commands[] = {

    {ngx_string("greetings"),           /* how we activate this module in config file */
     NGX_HTTP_LOC_CONF | NGX_CONF_NOARGS, /* module accepts no values */
     ngx_http_greetings,                       /* module configuration function */
     0,
     0,
     NULL},
    ngx_null_command /* end of commands */
};

static ngx_http_module_t ngx_http_greetings_module_ctx = {
    NULL, /* preconfiguration */
    NULL, /* postconfiguration */

    NULL, /* create main configuration */
    NULL, /* init main configuration */

    NULL, /* create server configuration */
    NULL, /* merge server configuration */

    NULL, /* create location configuration */
    NULL   /* merge location configuration */
};

/*
    the follwoing struct would be used by nginx to relate the code
    to the config file.
    Note that the name of this module should be the one you picked
    for $ngx_module_name.
    For this module, it is: $ngx_module_name=ngx_http_echo_module
*/
ngx_module_t ngx_http_greetings_module = {
    NGX_MODULE_V1,
    &ngx_http_greetings_module_ctx, /* module context */
    ngx_http_greetings_commands,    /*module directives*/
    NGX_HTTP_MODULE,           /* module type */
    NULL,                      /* init master */
    NULL,                      /* init module */
    NULL,                      /* init process */
    NULL,                      /* init thread */
    NULL,                      /* exit thread */
    NULL,                      /* exit process */
    NULL,                      /* exit master */
    NGX_MODULE_V1_PADDING};

static ngx_int_t ngx_http_greetings_handler(ngx_http_request_t *r)
{
    ngx_buf_t *b;
    ngx_chain_t out;
    ngx_str_t msg = ngx_string("greetings!");
 

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "http echo handler!");

    /* set response buffer for writing response  */
    b = ngx_pcalloc(r->pool, sizeof(ngx_buf_t));

    /* populate buffer chain. */
    out.buf = b;
    out.next = NULL; /* no more buffers */

    b->pos = msg.data;            
    b->last = msg.data + msg.len; 
    b->memory = 1;                
    b->last_buf = 1;              

    /* set output headers. */
    r->headers_out.status = NGX_HTTP_OK; 
    
    r->headers_out.content_length_n = msg.len;
    ngx_http_send_header(r); /* Send the headers */

    /* Send the body, and return the status code of the output filter chain. */
    return ngx_http_output_filter(r, &out);
}

static char *ngx_http_greetings(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t *clcf = conf; /* pointer to core location configuration */

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_greetings_handler;

    return NGX_CONF_OK;
}
