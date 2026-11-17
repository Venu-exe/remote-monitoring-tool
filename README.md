# Remote Monitoring Tool

A simple remote monitoring client and server tool written in Python. Built for learning purposes.

## Features

* Client collects system metrics (CPU, Memory, Disk, Network, Platform info) using `psutil`.
* Server can send commands to get the system metrics from the connected clients.

## Installation

1. Clone the repository.
2. Install the required dependencies:
   ```bash
   pip install -r requirements.txt
   ```

## Usage

### Run the Server
```bash
python server.py
```
The server will prompt for a host IP and port to bind to (defaults to 127.0.0.1:5000).

### Run the Client
```bash
python main.py
```
The client will prompt for the server IP and port to connect to.

Once connected, you can type commands in the server terminal, such as:
- `get_system_info`: Returns all system info.
- `get platform_info.system`: Returns a specific field from the system info.
- `exit`: Closes the connection.

## Example Output

### Client Connection
```text
Enter server IP [127.0.0.1]: 
Enter server port [5000]: 
Connected to server 127.0.0.1:5000
```

### Server Command & Output
```text
Accepted connection from 127.0.0.1:58432
Enter command to send to 127.0.0.1:58432 (or 'exit' to close connection): get platform_info.system
{'path': 'platform_info.system', 'value': 'Linux'}

Enter command to send to 127.0.0.1:58432 (or 'exit' to close connection): get memory_info.memory_percent
{'path': 'memory_info.memory_percent', 'value': 45.2}
```

---

# Advanced Monitoring Suite (v2)

A more advanced, multi-client version of the tool lives in:

| File | Role |
|---|---|
| `protocol.py` | Shared newline-delimited JSON protocol (`LineConnection`) with message size caps |
| `monitor_agent.py` | Agent: authenticates, streams metric heartbeats, answers read-only queries |
| `monitor_server.py` | Collector + interactive dashboard for many agents at once |
| `tests/test_monitoring.py` | Protocol unit tests + end-to-end agent/server tests |

## What makes it "advanced"

* **Multiple agents** — the server tracks many clients concurrently, each with a stable ID.
* **Token authentication** — agents must present a shared token in the handshake (`hmac.compare_digest`); bad tokens are rejected.
* **Optional TLS** — encrypt agent↔server traffic (see cert generation hint below).
* **Heartbeat streaming** — agents push metrics every `--interval` seconds; the server keeps a rolling history per agent.
* **Threshold alerts** — CPU / memory / disk thresholds trigger server-side alerts.
* **Request/response queries** — the dashboard can ask any agent for `ping`, `get_system_info`, or `get <dot.path>`, and can `broadcast` to all agents.
* **Resilience** — the agent reconnects with exponential backoff + jitter.

The agent is **read-only telemetry**: it exposes system metrics and answers queries — no remote command execution, file access, or persistence. Run it only on machines you own or are explicitly authorized to monitor.

## Usage

```bash
# 1. Start the collector (token via env var is recommended)
export MONITOR_TOKEN="$(openssl rand -hex 16)"
python monitor_server.py --host 0.0.0.0 --port 5000

# 2. On each monitored machine, start an agent
MONITOR_TOKEN="$MONITOR_TOKEN" python monitor_agent.py --host <server-ip> --port 5000 --interval 5

# 3. Optional TLS (generate a self-signed cert first)
openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem -out cert.pem -days 365 -subj '/CN=localhost'
python monitor_server.py --tls --certfile cert.pem --keyfile key.pem
MONITOR_TOKEN="$MONITOR_TOKEN" python monitor_agent.py --host <server-ip> --tls --cafile cert.pem
```

## Dashboard commands

```text
list                            show connected agents (cpu/mem/disk snapshot)
query <client_id> ping          ask one agent a read-only command
query <client_id> get memory_info.memory_percent
broadcast get_system_info       ask every agent at once
metrics <client_id>             latest heartbeat of an agent
history <client_id> 20          last 20 heartbeat samples
alerts                          show triggered threshold alerts
alerts clear                    clear the alert log
exit                            shut down
```

## Tests

```bash
pip install pytest
python -m pytest tests/ -v
```

The suite includes unauthorized-access tests: missing token, wrong token,
malformed JSON, non-object JSON, and oversized (>4 MiB) handshakes are all
rejected and the socket is closed (no file-descriptor leak).

### Testing unauthorized access yourself

```bash
# Terminal 1 — start the collector with a token
MONITOR_TOKEN=secret123 python monitor_server.py --port 5000

# Terminal 2 — try to connect WITHOUT a token (expect: denied, then closed)
printf '{"type":"hello","hostname":"attacker"}\n' | nc 127.0.0.1 5000

# wrong token -> same "denied" reply
printf '{"type":"hello","token":"letmein"}\n' | nc 127.0.0.1 5000

# garbage / oversized payload -> connection closed with no reply
printf 'hello server\n' | nc 127.0.0.1 5000
```

## Security notes

* The v2 server (`monitor_server.py`) requires a shared token on every
  handshake and caps message size at 4 MiB. Keep the token in the
  `MONITOR_TOKEN` env var, never hard-code it, and use `--tls` on untrusted
  networks.
* The original v1 pair (`server.py` / `main.py`) has **no authentication at
  all** — anyone who can reach the port can connect. Use v1 only on
  localhost/trusted networks, or migrate to the v2 suite.

---

# C++ Version (v3) — `cpp/`

A zero-dependency C++17 port of the v2 suite. **Same wire protocol**, so C++
and Python agents/servers interoperate (verified by `cpp/cross_test.py`).

| File | Role |
|---|---|
| `cpp/json.hpp` | Minimal JSON library (parse / dump / dump_pretty) — no dependencies |
| `cpp/protocol.{hpp,cpp}` | Newline-delimited JSON `LineConnection` with 4 MiB cap, dot-path lookup, threshold alerts |
| `cpp/metrics.{hpp,cpp}` | Linux metrics from `/proc` + syscalls (CPU deltas, meminfo, statvfs, net/dev, top processes) |
| `cpp/agent.cpp` | Agent binary: token handshake, heartbeats, read-only queries, backoff reconnect |
| `cpp/server.cpp` | Collector binary: multi-agent registry, dashboard console, alerts, history |
| `cpp/tests.cpp` | 32 unit tests (JSON, protocol, hostile input, metrics) |
| `cpp/cross_test.py` | 10 cross-language checks (C++↔Python interop) |

## Build & test

```bash
cd cpp
make            # builds build/agent and build/server
make test       # runs the 32 C++ unit tests
make cross      # runs the 10 cross-language tests (needs the Python suite)
```

## Usage

```bash
cd cpp
# Terminal 1 — collector (token via env var recommended)
MONITOR_TOKEN="$(openssl rand -hex 16)" ./build/server --host 0.0.0.0 --port 5000

# Terminal 2 — on each monitored machine
MONITOR_TOKEN="$MONITOR_TOKEN" ./build/agent --host <server-ip> --port 5000 --interval 5

# The console supports the same commands as the Python dashboard:
#   list | query <id> <cmd> | broadcast <cmd> | metrics <id> | history <id> | alerts | exit
```

The C++ agent is equally read-only: metrics + info queries only, token
authentication on every handshake, and rejected peers always get their socket
closed (no FD leak). Linux only (reads `/proc`). TLS is not wired up in the
C++ build yet — use it on trusted networks or behind the Python server's TLS.
