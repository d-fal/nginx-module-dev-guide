# ngx_modbus_gateway_module

A dynamic **nginx stream module** that turns nginx into a **Modbus TCP gateway**:
it inspects the *unit / slave identifier* in each incoming Modbus TCP request and
transparently proxies the connection to a different backend PLC/device depending
on that id.

It is a thin routing layer on top of nginx's built‑in `ngx_stream_proxy_module` —
the module only **selects** the backend; the stock proxy module does the actual
TCP proxying.

---

## Table of contents

- [What it does](#what-it-does)
- [How it works](#how-it-works)
- [Requirements & building](#requirements--building)
- [Loading the module](#loading-the-module)
- [Configuration](#configuration)
  - [Directive: `modbus`](#directive-modbus)
  - [Directive: `backend`](#directive-backend)
  - [Directive: `timeout`](#directive-timeout)
  - [Variable: `$modbus_backend`](#variable-modbus_backend)
  - [Required: `proxy_pass $modbus_backend;`](#required-proxy_pass-modbus_backend)
- [Complete example](#complete-example)
- [Routing logic](#routing-logic)
- [Protocol notes (read this if routing seems wrong)](#protocol-notes-read-this-if-routing-seems-wrong)
- [Testing](#testing)
- [Logging & troubleshooting](#logging--troubleshooting)
- [Limitations](#limitations)

---

## What it does

Modbus TCP devices are addressed by a **unit identifier** (the TCP equivalent of
the serial "slave address"), carried in the request header. A single TCP listener
traditionally maps to a single device.

This module lets one nginx listener fan out to **many** backend devices based on
that unit id:

```
                         ┌─────────────────────────────┐
 client ──TCP:502──▶     │  nginx + ngx_modbus_gateway │
 (unit id = 2)           │  peek unit id → pick backend│
                         └──────────────┬──────────────┘
                                        │ unit 2 → 10.20.20.2:502
                                        ▼
                                  PLC #2 (10.20.20.2)
```

The whole TCP byte stream (including the bytes the module peeked at) is forwarded
unchanged — the module never consumes or rewrites payload.

---

## How it works

1. **Preread phase.** Before proxying starts, a handler registered in
   `NGX_STREAM_PREREAD_PHASE` waits until at least the 7‑byte Modbus TCP MBAP
   header has been buffered, then reads the **unit id** (byte offset 6) *without
   consuming it*.
2. **Backend selection.** It looks the unit id up against the configured
   `modbus { }` blocks and stores the chosen backend address in the session.
3. **Variable resolution.** The chosen address is exposed through the
   `$modbus_backend` variable.
4. **Proxying.** The stock `ngx_stream_proxy_module` evaluates
   `proxy_pass $modbus_backend;` and connects to that backend, replaying the
   buffered bytes.
5. **Optional timeout.** If the matched block has a `timeout`, an absolute
   session timer is armed; when it fires, both the client and backend
   connections are closed (the same teardown path as the proxy's own
   `proxy_timeout`).

---

## Requirements & building

- nginx built **with the stream module** (`--with-stream`) and **compat ABI**
  (`--with-compat`) so the dynamic module can be loaded.
- The module is type `STREAM`; its addon name is `ngx_modbus_gateway_module`
  (see the `config` file).

Using the helper script in this repo (builds against the running nginx version
and copies the `.so` into nginx's modules directory):

```bash
./build_module.sh examples/05-modbus-gateway
```

Or manually:

```bash
cd nginx-<version>
./configure --with-compat --with-stream \
            --add-dynamic-module=../examples/05-modbus-gateway/
make modules
cp objs/ngx_modbus_gateway_module.so <nginx-modules-dir>/
```

---

## Loading the module

At the **top** of `nginx.conf` (main context):

```nginx
load_module modules/ngx_modbus_gateway_module.so;
```

---

## Configuration

All directives live inside a `stream { server { ... } }` block.

### Directive: `modbus`

```
Syntax:  modbus <slave_id> { ... }
         modbus default     { ... }
Context: stream > server
```

Opens a routing block for one unit id. Inside it you place `backend` (required)
and `timeout` (optional).

- `<slave_id>` — an integer in the range **0–255**.
- `default` — a catch‑all used when no exact match is found. (Internally this is
  stored as slave id `0`; see [Limitations](#limitations).)

You may declare as many `modbus` blocks as you need.

### Directive: `backend`

```
Syntax:  backend <host:port>;
Context: stream > server > modbus
```

The upstream device this unit id should be proxied to. The value is any address
the stream proxy module accepts (IP\:port, hostname\:port, or an `upstream {}`
name). **Required** inside every `modbus` block.

> ⚠️ `backend` is only meaningful **inside** a `modbus { }` block. Do not use it
> elsewhere.

### Directive: `timeout`

```
Syntax:  timeout <time>;
Context: stream > server > modbus
Default: (none — unlimited)
```

An **absolute maximum session duration** for connections routed to this backend.
Accepts standard nginx time units (`500ms`, `30s`, `2m`, `1h`). The timer starts
when the backend is selected (essentially at connection start). When it elapses,
the connection is closed **regardless of activity** — an in‑flight transfer is
cut. Omit it (or set `0`) for no limit.

> This is a hard lifetime cap, **not** an inactivity/idle timeout. For idle
> behaviour use the stream proxy module's own `proxy_timeout`.

### Variable: `$modbus_backend`

A read‑only stream variable set by the module to the `backend` address of the
matched block. It is empty (`not_found`) only if backend selection did not run or
found nothing.

### Required: `proxy_pass $modbus_backend;`

```nginx
proxy_pass $modbus_backend;
```

This is the stock `ngx_stream_proxy_module` directive and is **mandatory** at the
`server` level — it is what actually proxies the connection. Without it nginx
will refuse to start with:

```
nginx: [emerg] no handler for server in .../nginx.conf:NN
```

> Note the deliberate naming: the per‑device directive is called `backend`
> (not `proxy_pass`) precisely so it does not collide with this stock
> `proxy_pass` directive.

---

## Complete example

```nginx
load_module modules/ngx_modbus_gateway_module.so;

events {
    worker_connections 1024;
}

stream {
    server {
        listen 502;

        # Route by Modbus unit id.
        modbus 1 {
            backend 10.20.20.1:502;
            timeout 30s;
        }

        modbus 2 {
            backend 10.20.20.2:502;
            timeout 1m;
        }

        modbus 3 {
            backend 10.20.20.3:502;     # no timeout -> unlimited
        }

        # Catch-all for any unit id without an explicit block.
        modbus default {
            backend 10.20.20.254:502;
        }

        # Mandatory: the stock proxy module does the actual proxying.
        proxy_pass $modbus_backend;
    }
}
```

---

## Routing logic

For each connection, given the unit id `N` from the request header:

1. **Exact match** — if a `modbus N { }` block exists, route to its `backend`.
2. **Default** — otherwise, if a `modbus default { }` block exists, route there.
3. **No match** — if neither exists, the module logs an error and rejects the
   connection (`502`-class close):

   ```
   modbus_gateway: no backend for slave_id=N
   ```

---

## Protocol notes (read this if routing seems wrong)

This module parses **Modbus TCP**, where the request begins with a 7‑byte MBAP
header and the unit id is at **byte offset 6**:

```
│            MBAP header (7 bytes)            │   PDU …
[ Txn ID ][ Proto ID ][ Length ][ Unit ID ]  [ Func ][ Data … ]
   0  1      2   3      4   5       6           7      …
                                    ▲
                                    └─ unit id  ← routing byte
```

Do **not** confuse this with **Modbus RTU** (serial), where the slave address is
the *first* byte and the frame ends with a CRC. RTU framing is different:

```
[ Slave Addr ][ Func ][ Data … ][ CRC-16 ]      (byte 0 = address, no MBAP)
```

A genuine Modbus TCP frame has bytes **2–3 equal to `00 00`** (the Protocol ID).
If your devices send raw RTU frames inside TCP ("Modbus RTU over TCP"), the unit
id is at byte 0 and this module — which reads byte 6 — will route incorrectly.
That variant is **not** supported by this build.

---

## Testing

You can craft a valid *Read Holding Registers* request with `printf` and send it
with `nc`. Only the **7th byte** (unit id) affects routing.

```bash
# unit id 1  -> modbus 1
printf '\x00\x01\x00\x00\x00\x06\x01\x03\x00\x00\x00\x01' | nc 127.0.0.1 502

# unit id 2  -> modbus 2
printf '\x00\x01\x00\x00\x00\x06\x02\x03\x00\x00\x00\x01' | nc 127.0.0.1 502

# unit id 5  -> no exact block -> modbus default
printf '\x00\x01\x00\x00\x00\x06\x05\x03\x00\x00\x00\x01' | nc 127.0.0.1 502
```

Frame breakdown: `Txn=0001 Proto=0000 Len=0006 Unit=01 Func=03 Addr=0000 Qty=0001`.

**To exercise a `timeout`,** hold the connection open past the limit:

```bash
{ printf '\x00\x01\x00\x00\x00\x06\x01\x03\x00\x00\x00\x01'; sleep 40; } | nc 127.0.0.1 502
```

The connection should drop at the configured timeout. Note that the timeout can
only fire if the backend is actually reachable — if the backend connection fails,
nginx finalizes the session early with a connect error instead. For an isolated
timeout test, point a `backend` at a local sink:

```bash
nc -l 127.0.0.1 5020      # GNU netcat: add -k to keep listening
```

---

## Logging & troubleshooting

| Symptom | Cause / fix |
|---|---|
| `no handler for server` on startup | Missing `proxy_pass $modbus_backend;` at server level. |
| Connection closes immediately, log shows `no backend for slave_id=N` | No matching `modbus` block and no `modbus default`. |
| Connection closes early instead of timing out | The `backend` is unreachable — nginx fails the upstream connect before the timeout. |
| Routes to the wrong device | Likely RTU‑over‑TCP framing — see [Protocol notes](#protocol-notes-read-this-if-routing-seems-wrong). |

**Runtime** messages (backend selection, timeout) go to the `error_log` file:

```
modbus_gateway: slave_id=2 -> 10.20.20.2:502
modbus_gateway: session timeout reached, closing connection
```

**Config‑parse‑time** messages do **not** go to the `error_log` file — at parse
time nginx still logs to stderr. View them with a config test:

```bash
nginx -t -c /path/to/nginx.conf
```

---

## Limitations

- **Modbus TCP only.** Standard MBAP framing (unit id at byte 6). RTU and
  RTU‑over‑TCP are not supported.
- **`default` == unit id 0.** The catch‑all block is stored internally as slave
  id `0`. Since `0` is also the Modbus broadcast address, a request whose unit id
  is literally `0` is treated as "default".
- **`backend` / `timeout` are block‑scoped.** They must appear inside a
  `modbus { }` block; using them elsewhere is unsupported.
- **`timeout` is an absolute cap,** not an idle timeout.
- **Per‑server configuration.** Routing rules are configured per `server`. A
  `server` without its own `modbus` blocks inherits the rules from the
  enclosing/previous server configuration.
