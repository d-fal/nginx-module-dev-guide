# ngx_stream_xbcast_module

A minimal nginx **stream** module that demonstrates the *cross-worker wakeup*
mechanism an in-nginx pub/sub broker needs: when any worker receives a packet
whose first byte is `'X'`, it rings a "doorbell" to **every other worker** over a
private inter-process socketpair.

This is the smallest possible step toward a broker. It deliberately does **not**
carry a payload or deliver anything to a client — it only proves that worker A
can wake worker B. The next step (parking subscriber connections and fanning a
real payload out to them through shared memory) is shown in
[`../xpubsub-example`](../xpubsub-example).

## What it does, exactly

```
            ┌── nc "Xhello" ──► worker 0  (preread sees 'X')
                                   │
                                   │ ngx_write_channel() to every peer's socketpair
                                   ▼
                                worker 1  ── logs: "got 'X' event from pid ..."
```

1. A connection's first byte is inspected in the **preread phase**
   (`ngx_stream_xbcast_handler`).
2. If it is `'X'`, the worker calls `ngx_stream_xbcast_broadcast()`, which sends
   an `ngx_channel_t` (command `200`) to **every other worker**.
3. Each peer's custom channel read handler (`ngx_stream_xbcast_read_handler`)
   wakes up and logs that it received the event.
4. The original connection proceeds to the content phase and gets `"ok\n"`.

> **There is no subscriber socket.** The "notification" is an
> `error_log` line in the peer worker — *not* bytes sent back over a connection.
> To observe it you tail the error log. A version that delivers a payload to
> held-open subscriber connections is [`../xpubsub-example`](../xpubsub-example).

## Why a private socketpair instead of nginx's channel

nginx already has an inter-worker channel, but:

- `ngx_channel_handler` has a fixed command switch that **silently drops**
  unknown commands, and it is not hookable from a module.

So the module creates **its own** socketpairs and registers its **own** read
handler. It still reuses `ngx_write_channel` / `ngx_read_channel` /
`ngx_add_channel_event`, because those already frame an `ngx_channel_t` over a
`SOCK_STREAM` `AF_UNIX` socket with a single atomic `sendmsg`/`recvmsg`.

### The fd-inheritance trick

The socketpairs are created in `init_module()`, which runs in the **master,
before workers fork**. Every worker therefore inherits every fd:

```
channels[2*i]      = worker i's read end   (worker i listens here)
channels[2*i + 1]  = write end to worker i (any worker writes here to signal i)
```

To signal worker `i`, any worker writes to `channels[2*i + 1]`. Worker `i` is the
only process that reads `channels[2*i]`.

## Build

Built like any dynamic module in this repo (see the root `build_module.sh`),
inside the `nginx-costum` container:

```bash
# from the repo root, inside / against the nginx-costum container
./build_module.sh contrib/xbcast-example
```

The `config` file declares it as a `STREAM` module
(`ngx_module_type=STREAM`); nginx must be configured `--with-stream`
(`build_module.sh` already passes `--with-stream`).

## Run

```bash
nginx -c $(pwd)/contrib/xbcast-example/nginx.conf
```

Key bits of [`nginx.conf`](./nginx.conf):

- `worker_processes 2;` — you need **≥ 2 workers** to see a cross-worker hop.
- `listen 0.0.0.0:9000 reuseport;` — `reuseport` lets the kernel spread
  connections across workers, so two clients are likely to land on different
  workers.
- `stream_xbcast on;` — enables the preread handler for the server.
- `return "ok\n";` — content phase ack so each connection completes.
- `error_log /var/log/nginx/error.log notice;` — the notices below are at
  `NOTICE` level, so the log must be at `notice` or lower.

## Test

Because the notification is a log line, the "subscriber" is the error log.

```bash
# Terminal A — watch the notification
tail -f /var/log/nginx/error.log

# Terminal B — send a packet whose first byte is 'X'
printf 'Xhello' | nc -q1 127.0.0.1 9000
```

Expected in the log — the sending worker, then a *different* worker reacting:

```
xbcast: worker 0 saw 'X' packet, broadcasting
xbcast: worker 1 got 'X' event from pid 1234 (would read shm slot 0 and fan out to subscribers)
```

Notes:

- The `'X'` must be in the **first packet** — `printf` in one shot guarantees it.
- A first byte other than `'X'` is a no-op (still gets `ok\n`).
- With a single worker there are no peers to signal, so nothing is logged on the
  receive side (the module skips IPC setup entirely when `worker_processes < 2`).

## Directive

| Directive       | Context                       | Default | Description                          |
| --------------- | ----------------------------- | ------- | ------------------------------------ |
| `stream_xbcast` | `stream`, `server` (`on`/`off`) | `off`   | Enable the `'X'`-packet doorbell.    |

## Known shortcuts (it's a teaching example)

- **No payload.** `ch.slot` is set to `ngx_process_slot` only to *show where* a
  real broker would stash an index into a shared-memory message store. Nothing
  is actually read from it.
- **No backpressure handling.** If a peer's socket buffer is full,
  `ngx_write_channel` returns `NGX_AGAIN`; the module just logs a warning and
  drops the alert. A real broker must queue and retry on writability.
- **No delivery to clients.** See [`../xpubsub-example`](../xpubsub-example) for
  the version that parks subscriber connections and fans a real payload out to
  them via a refcounted shared-memory slab.

## Files

| File                          | Purpose                                  |
| ----------------------------- | ---------------------------------------- |
| `ngx_stream_xbcast_module.c`  | The module.                              |
| `nginx.conf`                  | Minimal 2-worker stream config for testing. |
| `config`                      | nginx add-on build descriptor.           |
