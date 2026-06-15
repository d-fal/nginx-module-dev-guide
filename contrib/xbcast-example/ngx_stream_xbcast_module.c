/*
 * Minimal example: a stream module that, whenever it receives a packet whose
 * first byte is 'X', sends an event to *every other worker* over a private
 * inter-process socketpair ("the doorbell").
 *
 * The packet contents are irrelevant here -- this only demonstrates the
 * cross-worker wakeup mechanism that an in-nginx pub/sub broker needs. A real
 * broker would carry an index into a shared-memory message store in the event
 * (we stash ngx_process_slot in ch.slot to show where that would go) and the
 * receiving worker would then fan the message out to its own subscriber
 * sockets -- the only sockets whose fds it actually owns.
 *
 * Design notes:
 *   - We do NOT reuse nginx's built-in channel: its ngx_channel_handler has a
 *     fixed command switch that silently drops unknown commands, and it is not
 *     hookable from a module. So we create our own socketpairs and register our
 *     own read handler. We *do* reuse ngx_write_channel/ngx_read_channel/
 *     ngx_add_channel_event because they already frame an ngx_channel_t over a
 *     SOCK_STREAM AF_UNIX socket with a single atomic sendmsg/recvmsg.
 *   - The socketpairs are created in init_module(), which runs in the master
 *     BEFORE workers fork, so every worker inherits every fd. Worker i listens
 *     on pair[i][0]; to signal worker i, any worker writes to pair[i][1].
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_stream.h>
#include <ngx_channel.h>


/* private channel command; kept clear of the core NGX_CMD_* range */
#define NGX_STREAM_XBCAST_CMD  200


typedef struct {
    ngx_flag_t  enabled;
} ngx_stream_xbcast_srv_conf_t;


/*
 * One socketpair per worker, created in the master before fork.
 * Flattened: channels[2*i] = worker i's read end, channels[2*i + 1] = the
 * write end any worker uses to signal worker i.
 */
static ngx_socket_t  *xbcast_channels;
static ngx_uint_t     xbcast_nworkers;

/* this worker's own number, or -1 when IPC is not active (single process) */
static ngx_int_t      xbcast_my_worker = -1;


static ngx_int_t ngx_stream_xbcast_handler(ngx_stream_session_t *s);
static void ngx_stream_xbcast_broadcast(ngx_log_t *log);
static void ngx_stream_xbcast_read_handler(ngx_event_t *ev);

static ngx_int_t ngx_stream_xbcast_postconf(ngx_conf_t *cf);
static ngx_int_t ngx_stream_xbcast_init_module(ngx_cycle_t *cycle);
static ngx_int_t ngx_stream_xbcast_init_process(ngx_cycle_t *cycle);

static void *ngx_stream_xbcast_create_srv_conf(ngx_conf_t *cf);
static char *ngx_stream_xbcast_merge_srv_conf(ngx_conf_t *cf,
    void *parent, void *child);


static ngx_command_t  ngx_stream_xbcast_commands[] = {

    { ngx_string("stream_xbcast"),
      NGX_STREAM_MAIN_CONF | NGX_STREAM_SRV_CONF | NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_STREAM_SRV_CONF_OFFSET,
      offsetof(ngx_stream_xbcast_srv_conf_t, enabled),
      NULL },

    ngx_null_command
};


static ngx_stream_module_t  ngx_stream_xbcast_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_stream_xbcast_postconf,            /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    ngx_stream_xbcast_create_srv_conf,     /* create server configuration */
    ngx_stream_xbcast_merge_srv_conf       /* merge server configuration */
};


ngx_module_t  ngx_stream_xbcast_module = {
    NGX_MODULE_V1,
    &ngx_stream_xbcast_module_ctx,         /* module context */
    ngx_stream_xbcast_commands,            /* module directives */
    NGX_STREAM_MODULE,                     /* module type */
    NULL,                                  /* init master */
    ngx_stream_xbcast_init_module,         /* init module (master, pre-fork) */
    ngx_stream_xbcast_init_process,        /* init process (each worker) */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


/* ---- the stream side: detect the 'X' packet in the preread phase ---- */

static ngx_int_t
ngx_stream_xbcast_handler(ngx_stream_session_t *s)
{
    ngx_connection_t              *c;
    ngx_buf_t                     *b;
    ngx_stream_xbcast_srv_conf_t  *xscf;

    xscf = ngx_stream_get_module_srv_conf(s, ngx_stream_xbcast_module);

    if (!xscf->enabled) {
        return NGX_OK;
    }

    c = s->connection;
    b = c->buffer;

    /* preread phase: wait until we have at least the first byte */
    if (b == NULL || b->last == b->pos) {
        return NGX_AGAIN;
    }

    if (b->pos[0] == 'X') {
        ngx_log_error(NGX_LOG_NOTICE, c->log, 0,
                      "xbcast: worker %ui saw 'X' packet, broadcasting",
                      ngx_worker);
        ngx_stream_xbcast_broadcast(c->log);
    }

    return NGX_OK;
}


static void
ngx_stream_xbcast_broadcast(ngx_log_t *log)
{
    ngx_uint_t     i;
    ngx_channel_t  ch;

    if (xbcast_my_worker < 0) {
        /* single-process mode: no peers to signal */
        return;
    }

    ngx_memzero(&ch, sizeof(ngx_channel_t));
    ch.command = NGX_STREAM_XBCAST_CMD;
    ch.pid = ngx_pid;
    ch.slot = ngx_process_slot;   /* real broker: index of the msg in shm */
    ch.fd = -1;                   /* we are not passing an fd */

    for (i = 0; i < xbcast_nworkers; i++) {

        if (i == (ngx_uint_t) xbcast_my_worker) {
            continue;             /* don't signal ourselves */
        }

        /* write to peer i's read end (its [0]) by writing to its [1] */
        if (ngx_write_channel(xbcast_channels[2 * i + 1], &ch,
                              sizeof(ngx_channel_t), log)
            != NGX_OK)
        {
            /*
             * NGX_AGAIN here means peer i's socket buffer is full (it is
             * non-blocking). A real broker must queue and retry on writability
             * instead of dropping the alert.
             */
            ngx_log_error(NGX_LOG_WARN, log, 0,
                          "xbcast: could not signal worker %ui", i);
        }
    }
}


/* ---- the receiving side: our own channel read handler, per worker ---- */

static void
ngx_stream_xbcast_read_handler(ngx_event_t *ev)
{
    ngx_int_t          n;
    ngx_channel_t      ch;
    ngx_connection_t  *c;

    if (ev->timedout) {
        ev->timedout = 0;
        return;
    }

    c = ev->data;

    for ( ;; ) {

        n = ngx_read_channel(c->fd, &ch, sizeof(ngx_channel_t), ev->log);

        if (n == NGX_ERROR) {
            ngx_close_connection(c);
            return;
        }

        /* event ports are oneshot; re-arm like nginx's own channel handler */
        if (ngx_event_flags & NGX_USE_EVENTPORT_EVENT) {
            if (ngx_add_event(ev, NGX_READ_EVENT, 0) == NGX_ERROR) {
                return;
            }
        }

        if (n == NGX_AGAIN) {
            return;
        }

        if (ch.command == NGX_STREAM_XBCAST_CMD) {
            ngx_log_error(NGX_LOG_NOTICE, ev->log, 0,
                          "xbcast: worker %ui got 'X' event from pid %P "
                          "(would read shm slot %i and fan out to subscribers)",
                          ngx_worker, ch.pid, ch.slot);
        }
    }
}


/* ---- lifecycle wiring ---- */

static ngx_int_t
ngx_stream_xbcast_postconf(ngx_conf_t *cf)
{
    ngx_stream_handler_pt        *h;
    ngx_stream_core_main_conf_t  *cmcf;

    cmcf = ngx_stream_conf_get_module_main_conf(cf, ngx_stream_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_STREAM_PREREAD_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_stream_xbcast_handler;

    return NGX_OK;
}


static ngx_int_t
ngx_stream_xbcast_init_module(ngx_cycle_t *cycle)
{
    ngx_uint_t        i;
    ngx_core_conf_t  *ccf;

    /* runs in the master before fork (and again on reload) */

    if (xbcast_channels != NULL) {
        /* already created in a previous cycle; keep the existing fds */
        return NGX_OK;
    }

    ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);

    xbcast_nworkers = (ccf->worker_processes > 0)
                      ? (ngx_uint_t) ccf->worker_processes : 1;

    if (xbcast_nworkers < 2) {
        /* one process only: nobody to signal, skip IPC setup entirely */
        return NGX_OK;
    }

    xbcast_channels = ngx_palloc(cycle->pool,
                                 sizeof(ngx_socket_t) * 2 * xbcast_nworkers);
    if (xbcast_channels == NULL) {
        return NGX_ERROR;
    }

    for (i = 0; i < xbcast_nworkers; i++) {

        if (socketpair(AF_UNIX, SOCK_STREAM, 0, &xbcast_channels[2 * i]) == -1) {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                          "xbcast: socketpair() failed");
            return NGX_ERROR;
        }

        /* match nginx's own channels: non-blocking + close-on-exec */
        if (ngx_nonblocking(xbcast_channels[2 * i]) == -1
            || ngx_nonblocking(xbcast_channels[2 * i + 1]) == -1)
        {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                          ngx_nonblocking_n " failed for xbcast channel");
            return NGX_ERROR;
        }

        if (fcntl(xbcast_channels[2 * i], F_SETFD, FD_CLOEXEC) == -1
            || fcntl(xbcast_channels[2 * i + 1], F_SETFD, FD_CLOEXEC) == -1)
        {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                          "xbcast: fcntl(FD_CLOEXEC) failed");
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}


static ngx_int_t
ngx_stream_xbcast_init_process(ngx_cycle_t *cycle)
{
    /* runs in every spawned process after fork */

    if (xbcast_channels == NULL) {
        return NGX_OK;                  /* single-process mode */
    }

    /* only real workers participate; skip cache manager/loader helpers */
    if (ngx_process != NGX_PROCESS_WORKER) {
        return NGX_OK;
    }

    if (ngx_worker >= xbcast_nworkers) {
        return NGX_OK;                  /* defensive: shouldn't happen */
    }

    /* listen on our own read end with our custom handler */
    if (ngx_add_channel_event(cycle, xbcast_channels[2 * ngx_worker],
                              NGX_READ_EVENT,
                              ngx_stream_xbcast_read_handler)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    xbcast_my_worker = (ngx_int_t) ngx_worker;

    return NGX_OK;
}


static void *
ngx_stream_xbcast_create_srv_conf(ngx_conf_t *cf)
{
    ngx_stream_xbcast_srv_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_stream_xbcast_srv_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    conf->enabled = NGX_CONF_UNSET;

    return conf;
}


static char *
ngx_stream_xbcast_merge_srv_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_stream_xbcast_srv_conf_t  *prev = parent;
    ngx_stream_xbcast_srv_conf_t  *conf = child;

    ngx_conf_merge_value(conf->enabled, prev->enabled, 0);

    return NGX_CONF_OK;
}
