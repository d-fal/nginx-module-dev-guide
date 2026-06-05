# ngx_stream_modbus_proxy_module

A dynamic **nginx stream module** that turns nginx into a **Modbus gateway**. It
inspects the *unit / slave identifier* in each incoming Modbus TCP request and,
depending on that id, either:

- **`mode tcp`** — transparently proxies the connection to a backend PLC/device
  over TCP (a thin routing layer on top of nginx's built‑in
  `ngx_stream_proxy_module` — the module only **selects** the backend), or
- **`mode rtu`** — bridges the request onto a **serial line** (Modbus RTU) using
  **libmodbus**, translating Modbus TCP ⇄ Modbus RTU on the fly.

Both modes can coexist on the same listener: some unit ids proxy to TCP devices,
others are bridged to slaves on a shared serial bus.

---

## Table of contents

- [What it does](#what-it-does)
- [How it works](#how-it-works)
- [Requirements & building](#requirements--building)
- [Loading the module](#loading-the-module)
- [Configuration](#configuration)
  - [Directive: `modbus`](#directive-modbus)
  - [Directive: `host`](#directive-host)
  - [Directive: `timeout`](#directive-timeout)
  - [Directive: `deny_ops`](#directive-deny_ops)
  - [RTU serial-bus directives](#rtu-serial-bus-directives)
  - [Variable: `$proxy_pass`](#variable-proxy_pass)
  - [Required: `proxy_pass $proxy_pass;`](#required-proxy_pass-proxy_pass)
- [Complete example](#complete-example)
- [Routing logic](#routing-logic)
- [The RTU bridge in detail](#the-rtu-bridge-in-detail)
- [Protocol notes](#protocol-notes)
- [Testing](#testing)
- [Logging & troubleshooting](#logging--troubleshooting)
- [Limitations](#limitations)

---

## What it does

Modbus devices are addressed by a **unit identifier** (the TCP equivalent of the
serial "slave address"). In Modbus TCP it is carried in the request header; on a
Modbus RTU serial bus it is the first byte on the wire.

This module lets one nginx TCP listener fan out to **many** backends based on that
unit id — and each backend can be either another TCP device or a slave on a serial
line:

```
                          ┌──────────────────────────────────┐
 client ──TCP:502──▶      │  nginx + ngx_stream_modbus_proxy   │
                          │  peek unit id → pick backend/mode  │
                          └───────┬───────────────────┬────────┘
              unit 1 (mode tcp)   │                   │  unit 5 (mode rtu)
                                  ▼                   ▼
                           PLC #1 (10.20.20.1)   serial bus /dev/ttyUSB0
                              TCP proxy           (RTU slave addr = 5)
```

On the **tcp** path the whole TCP byte stream is forwarded unchanged. On the
**rtu** path the module terminates the TCP connection itself and speaks RTU on the
serial side, rebuilding a Modbus TCP (MBAP) reply for the client.

---

## How it works

1. **Preread phase.** A handler registered in `NGX_STREAM_PREREAD_PHASE` waits
   until at least the 7‑byte Modbus TCP MBAP header is buffered, then reads the
   **unit id** (byte offset 6) *without consuming it*.
2. **Backend selection.** The unit id is matched against the configured
   `modbus { }` blocks to pick a block (or the `default` block).
3. **Op filter (optional).** If the matched block has [`deny_ops`](#directive-deny_ops),
   the request's function code is checked against it; a denied op closes the
   connection (`403`) before any forwarding.
4. **Dispatch by mode:**
   - **`mode tcp`** — the chosen backend address is exposed via the `$proxy_pass`
     variable; the stock `ngx_stream_proxy_module` evaluates
     `proxy_pass $proxy_pass;`, connects, and replays the buffered bytes.
   - **`mode rtu`** — the module **takes over the client connection** (returns
     `NGX_DONE` from the preread phase so the TCP proxy never runs) and drives the
     RTU bridge described [below](#the-rtu-bridge-in-detail).
5. **Optional timeout.** If the matched block has a `timeout`, an absolute session
   timer is armed (on either path); when it fires the session is closed.

---

## Requirements & building

- nginx built **with the stream module** (`--with-stream`) and the **compat ABI**
  (`--with-compat`) so the dynamic module can be loaded.
- **libmodbus** development files (`libmodbus-dev` on Debian/Ubuntu,
  `libmodbus` on Arch). The module's `config` resolves the include/lib flags via
  `pkg-config` and links `-lmodbus` into the `.so`.
- The module is type `STREAM`; its addon name is `ngx_stream_modbus_proxy_module`.

> **Dev-container note:** the repo's `docker/Dockerfile` does **not** install
> libmodbus yet — run `apt-get install -y libmodbus-dev` in the container before
> building, or add it to the Dockerfile.

Using the helper script in this repo (builds against the running nginx version and
copies the `.so` into nginx's modules directory):

```bash
./build_module.sh examples/05-modbus-gateway
```

Or manually:

```bash
cd nginx-<version>
./configure --with-compat --with-stream \
            --add-dynamic-module=../examples/05-modbus-gateway/
make modules
cp objs/ngx_stream_modbus_proxy_module.so <nginx-modules-dir>/
```

---

## Loading the module

At the **top** of `nginx.conf` (main context):

```nginx
load_module modules/ngx_stream_modbus_proxy_module.so;
```

---

## Configuration

Routing blocks and the RTU serial-bus parameters live inside
`stream { server { ... } }`.

### Directive: `modbus`

```
Syntax:  modbus <slave_id> <mode> { ... }
         modbus default     <mode> { ... }
Context: stream > server
```

Opens a routing block for one unit id. Both arguments are **required**:

- `<slave_id>` — an integer in the range **0–255**.
- `default` — a catch‑all used when no exact match is found. (Internally stored as
  slave id `0`; see [Limitations](#limitations).)
- `<mode>` — `tcp` or `rtu` (mandatory):
  - `tcp` — proxy this unit id to its `host` over TCP.
  - `rtu` — bridge this unit id to the [serial bus](#rtu-serial-bus-directives)
    configured at the server level; the request's unit id is the RTU slave
    address on the wire.

  A missing or invalid mode is rejected at `nginx -t` (`invalid number of
  arguments` / `invalid modbus mode "…": must be tcp or rtu`).

Inside the block you place `host` (required for tcp) and optionally `timeout`. You
may declare as many `modbus` blocks as you need.

```nginx
modbus 1 tcp { host 10.20.20.1:502; }   # TCP proxy
modbus 5 rtu { }                        # bridged to the serial bus
```

### Directive: `host`

```
Syntax:  host <host:port>;
Context: stream > server > modbus
```

The upstream TCP device this unit id should be proxied to. Any address the stream
proxy accepts (IP\:port, hostname\:port, or an `upstream {}` name). **Required for
`tcp` blocks**; ignored by `rtu` blocks (an rtu unit's address is the slave id on
the serial bus).

### Directive: `timeout`

```
Syntax:  timeout <time>;
Context: stream > server > modbus
Default: (none — unlimited)
```

An **absolute maximum session duration** for connections routed to this block.
Accepts standard nginx time units (`500ms`, `30s`, `2m`, `1h`). When it elapses
the connection is closed **regardless of activity**. Applies to both tcp and rtu
blocks. Omit it (or set `0`) for no limit.

> This is a hard lifetime cap, **not** an inactivity/idle timeout. For idle
> behaviour on the tcp path use the stream proxy module's own `proxy_timeout`.

### Directive: `deny_ops`

```
Syntax:  deny_ops <op> [<op> ...];
Context: stream > server > modbus
Default: (none — all function codes allowed)
```

An **allow/deny filter on Modbus function codes** for this block. Each request's
function code (PDU byte, MBAP offset 7) is checked against the list **before**
forwarding; a denied op never reaches the backend. Applies to **both `tcp` and
`rtu`** blocks.

Takes **one or more** op tokens (`NGX_CONF_1MORE`). The tokens are folded into a
bitmask at config load, so:

- listing several ops on one line — `deny_ops read_coils write_coil;` — denies all
  of them;
- repeating the directive accumulates (it does **not** reset); a repeated token
  just logs a `duplicate value` warning;
- an **unknown token aborts startup** at `nginx -t` with `invalid value "…"`, so
  typos are caught at load time rather than silently ignored.

**Recognised ops** (token → Modbus function code):

| Token                  | Func code | Operation                       |
|------------------------|-----------|---------------------------------|
| `read_coils`           | `0x01`    | Read Coils                      |
| `read_discrete`        | `0x02`    | Read Discrete Inputs            |
| `read_holding`         | `0x03`    | Read Holding Registers          |
| `read_input_reg`       | `0x04`    | Read Input Registers            |
| `write_coil`           | `0x05`    | Write Single Coil               |
| `write_reg`            | `0x06`    | Write Single Register           |
| `write_multiple_bits`  | `0x0F`    | Write Multiple Coils            |
| `write_multiple_regs`  | `0x10`    | Write Multiple Registers        |

When a request carries a denied op the module logs it and closes the connection
with a `403`‑class status (`NGX_STREAM_FORBIDDEN`):

```
modbus_proxy: denied op 0x05 for slave_id=1
```

```nginx
modbus 1 tcp {
    host 10.20.20.1:502;
    deny_ops write_coil write_reg write_multiple_bits write_multiple_regs;  # read-only device
}
```

> **Note:** denial currently **drops the TCP connection** (403) rather than
> returning a Modbus *Illegal Function* (`0x01`) exception PDU. A Modbus master
> will therefore see the socket close, not a protocol‑level error.

### RTU serial-bus directives

A single RS‑485 line carries **many** slave addresses, so the serial parameters
describe the **bus**, not any one slave. They are therefore set at the **`server`
level — outside** the `modbus { }` blocks. They are required only if at least one
block uses `mode rtu`.

```
Context: stream > server
```

| Directive          | Values                | Default | Notes |
|--------------------|-----------------------|---------|-------|
| `serial`           | device path           | —       | e.g. `/dev/ttyUSB0`. **Required** if any rtu block exists. |
| `baud`             | integer               | `9600`  | e.g. `9600`, `19200`, `115200`. |
| `data_bits`        | `7` \| `8`            | `8`     | |
| `parity`           | `none` \| `even` \| `odd` | `none` | |
| `stop_bits`        | `1` \| `2`            | `1`     | |
| `response_timeout` | time                  | `1s`    | How long to wait for an RTU reply before returning a gateway exception. |

Invalid values are rejected at `nginx -t` (e.g. `"data_bits" must be 7 or 8`).

### Variable: `$proxy_pass`

A read‑only stream variable set by the module to the `host` address of the matched
block. Used to feed the stock proxy on the tcp path. It is empty (`not_found`) for
rtu blocks or when selection did not run.

### Required: `proxy_pass $proxy_pass;`

```nginx
proxy_pass $proxy_pass;
```

This stock `ngx_stream_proxy_module` directive is **mandatory** at the `server`
level — it is the server's content handler and is what proxies tcp‑mode
connections. It is still required even if every block is `mode rtu` (otherwise
nginx refuses to start with `no handler for server …`); rtu connections simply
never reach it, because the module takes them over in the preread phase.

---

## Complete example

```nginx
load_module modules/ngx_stream_modbus_proxy_module.so;

events {
    worker_connections 1024;
}

stream {
    server {
        listen 502;

        # --- RTU serial-bus parameters (server level, shared by all rtu units) ---
        serial           /dev/ttyUSB0;
        baud             9600;
        data_bits        8;
        parity           none;
        stop_bits        1;
        response_timeout 1s;

        # --- Routing blocks: modbus <slave_id> [mode] { ... } ---
        modbus 1 tcp {                   # TCP device
            host 10.20.20.1:502;
            timeout 30s;
            deny_ops write_coil write_reg # expose this unit read-only
                     write_multiple_bits write_multiple_regs;
        }

        modbus 5 rtu {                   # RTU slave on /dev/ttyUSB0, address 5
        }

        modbus default tcp {             # catch-all (TCP)
            host 10.20.20.254:502;
        }

        # Mandatory: content handler; proxies tcp-mode connections.
        proxy_pass $proxy_pass;
    }
}
```

---

## Routing logic

For each connection, given the unit id `N` from the request header:

1. **Exact match** — if a `modbus N { }` block exists, use it.
2. **Default** — otherwise, if a `modbus default { }` block exists, use it.
3. **No match** — if neither exists, the module logs an error and closes the
   connection (`502`‑class):

   ```
   modbus_proxy: no backend for slave_id=N
   ```

The selected block's `mode` then decides tcp proxy vs. rtu bridge.

On a persistent rtu connection each request is **re‑routed by its own unit id**, so
a single client connection can address several rtu slaves on the bus. If a request
targets a unit that is not an rtu block, the bridge returns a Modbus exception
rather than dropping the connection (see below).

---

## The RTU bridge in detail

When the selected block is `mode rtu`, the module takes over the client socket and
runs this loop per request, entirely on nginx's event loop except for the serial
round‑trip:

```
read full MBAP request  ─▶  build RTU frame (libmodbus adds the CRC)
        ▲                            │
        │                            ▼  modbus_send_raw_request  (blocking)
   next request                serial bus
        │                            │  modbus_receive_confirmation (blocking,
        │                            ▼   up to response_timeout)
   write MBAP reply  ◀──  validate + strip CRC, rebuild MBAP (echo txn id)
```

Key properties:

- **libmodbus owns the serial protocol** — CRC, framing, and the reply timeout.
  The libmodbus context is opened **lazily on the first rtu request** and kept open
  for the worker's lifetime (reopened after a serial error). One context is shared
  per serial bus; `modbus_set_slave()` selects the target before each transaction.
- **Blocking, by design.** The serial round‑trip blocks the worker for up to
  `response_timeout`. This is acceptable for a low‑concurrency gateway (the example
  runs with `master_process off`); it is **not** suited to high client concurrency
  on one worker.
- **Persistent connection.** Real Modbus TCP masters reuse the connection — the
  bridge serves many request/response pairs until the client closes.
- **Failures become Modbus exceptions** (the connection stays alive):

  | Exception | Code | When |
  |-----------|------|------|
  | Gateway path unavailable        | `0x0A` | the request's unit id has no rtu block |
  | Gateway target failed to respond | `0x0B` | serial open/send failed, or no/garbled/timed‑out reply |

  The reply is a standard Modbus exception PDU: function code `| 0x80`, then the
  exception code, wrapped in an MBAP header echoing the client's transaction id.

---

## Protocol notes

The module parses **Modbus TCP** on the client side, where the request begins with
a 7‑byte MBAP header and the unit id is at **byte offset 6**:

```
│            MBAP header (7 bytes)            │   PDU …
[ Txn ID ][ Proto ID ][ Length ][ Unit ID ]  [ Func ][ Data … ]
   0  1      2   3      4   5       6           7      …
                                    ▲
                                    └─ unit id  ← routing byte
```

A genuine Modbus TCP frame has bytes **2–3 equal to `00 00`** (Protocol ID). On the
rtu bridge path the module rejects frames with a non‑zero protocol id; on the tcp
proxy path the bytes are forwarded unchanged. "Modbus RTU over TCP" (raw RTU frames
inside TCP, unit id at byte 0, trailing CRC) is **not** supported on the client
side.

On the **rtu** path the on‑wire serial frame is standard Modbus RTU
(`[addr][func][data…][CRC‑16]`); this framing is produced/consumed by libmodbus,
not by hand.

---

## Testing

### TCP routing

Craft a valid *Read Holding Registers* request with `printf` and send it with `nc`.
Only the 7th byte (unit id) affects routing:

```bash
# unit id 1 -> modbus 1
printf '\x00\x01\x00\x00\x00\x06\x01\x03\x00\x00\x00\x01' | nc 127.0.0.1 502
```

Frame breakdown: `Txn=0001 Proto=0000 Len=0006 Unit=01 Func=03 Addr=0000 Qty=0001`.

To exercise `deny_ops`, send a request whose **func byte (offset 7)** is a denied
op. With `deny_ops write_coil;` on unit 1, a *Write Single Coil* (`0x05`) is
dropped (connection closes, log shows `denied op 0x05 for slave_id=1`), while a
*Read Holding Registers* (`0x03`) still succeeds:

```bash
# denied: Write Single Coil (func 0x05) -> connection closed
printf '\x00\x01\x00\x00\x00\x06\x01\x05\x00\x00\xFF\x00' | nc 127.0.0.1 502
```

### RTU bridge (no hardware required)

Use a virtual serial pair and a fake RTU slave. With Python's `pty` you can open a
pair, point `serial` at the slave path, and answer requests on the master fd; or
use `socat`:

```bash
socat -d -d pty,raw,echo=0,link=/tmp/ttyA pty,raw,echo=0,link=/tmp/ttyB
# point `serial /tmp/ttyA;` in nginx.conf, run an RTU slave on /tmp/ttyB
```

Then send a Modbus TCP request whose unit id matches an `mode rtu` block and check
that the reply echoes the transaction id and carries the expected PDU. Useful
assertions: a missing slave reply yields exception `0x0B` after `response_timeout`;
a unit id with no rtu block yields exception `0x0A`.

---

## Logging & troubleshooting

| Symptom | Cause / fix |
|---|---|
| `no handler for server` on startup | Missing `proxy_pass $proxy_pass;` at server level. |
| `nginx -t`: `… requires a server-level "serial" directive` | An `mode rtu` block exists but no `serial` is set at server level. |
| `nginx -t`: `"serial" is duplicate` at the serial line | The loaded `.so` predates moving the serial params to server level — **rebuild the module**. |
| Connection closes, log shows `no backend for slave_id=N` | No matching `modbus` block and no `modbus default`. |
| Connection closes, log shows `denied op 0xNN for slave_id=N` | The request's function code is listed in the block's `deny_ops` — expected; remove the op from `deny_ops` to allow it. |
| `nginx -t`: `invalid value "…"` near `deny_ops` | Unknown op token — see the [recognised ops table](#directive-deny_ops). |
| Client gets a Modbus exception `0x0B` | RTU slave didn't answer in `response_timeout`, or the serial port failed to open — check wiring / `serial` path / baud. |
| Client gets a Modbus exception `0x0A` | The addressed unit id isn't an `mode rtu` block. |

**Runtime** messages go to the `error_log` file:

```
modbus_proxy: opened serial /dev/ttyUSB0 @ 9600 baud
modbus_proxy: unit 9 has no rtu backend
modbus_proxy: no/garbled RTU reply: Connection timed out
```

**Config‑parse‑time** messages (and validation errors) go to **stderr**, not the
`error_log` — view them with:

```bash
nginx -t -c /path/to/nginx.conf
```

---

## Limitations

- **Client side is Modbus TCP only** (MBAP framing, unit id at byte 6). RTU and
  RTU‑over‑TCP are not accepted from clients; rtu is only a *bridge target*.
- **The RTU bridge is blocking.** Each serial transaction stalls the worker for up
  to `response_timeout`; fine for low concurrency, not for many simultaneous
  clients per worker. (A thread‑pool design was intentionally avoided.)
- **One serial bus per server.** `serial` and the line parameters are
  server‑scoped; all rtu blocks on a server share one bus and one libmodbus
  context.
- **`default` == unit id 0.** The catch‑all is stored internally as slave id `0`.
  Since `0` is also the Modbus broadcast address, a request whose unit id is
  literally `0` is treated as "default".
- **Directive scoping.** `mode` is a required positional argument of `modbus`;
  `host` / `timeout` are block‑scoped; the serial parameters are server‑scoped.
  Placing them in the wrong scope is unsupported.
- **`timeout` is an absolute cap,** not an idle timeout.
- **`deny_ops` drops the connection** (403) instead of replying with a Modbus
  *Illegal Function* (`0x01`) exception. The filter is per‑function‑code only — it
  does not inspect addresses, quantities, or payloads.
- **Per‑server configuration.** A `server` without its own `modbus` blocks inherits
  the rules from the enclosing/previous server configuration.
