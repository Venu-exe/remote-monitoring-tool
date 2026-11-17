#!/usr/bin/env python3
"""Cross-language interoperability test (requires `make` first).

  Test A: C++ agent  -> real Python CollectorServer (monitor_server.py)
  Test B: raw Python client -> C++ server binary (build/server)

The wire protocol is identical, so both directions must work.
"""

import json
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

CPP = ROOT / "cpp"
AGENT_BIN = CPP / "build" / "agent"
SERVER_BIN = CPP / "build" / "server"
TOKEN = "cross-token"

from monitor_server import CollectorServer  # noqa: E402

results = []


def check(name, cond):
    print(("PASS " if cond else "FAIL ") + name)
    results.append(bool(cond))


def recv_msg(sock, buf=b""):
    while b"\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            return None, buf
        buf += chunk
    line, buf = buf.split(b"\n", 1)
    return json.loads(line), buf


def connect_with_retry(port, tries=50):
    for _ in range(tries):
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=5)
        except OSError:
            time.sleep(0.1)
    raise RuntimeError("server not reachable")


# ---------------------------------------------------------------- Test A
print("--- Test A: C++ agent -> Python CollectorServer ---")
server = CollectorServer("127.0.0.1", 0, token=TOKEN,
                          thresholds={"memory.percent": 0.0}).start()
proc = subprocess.Popen(
    [str(AGENT_BIN), "--host", "127.0.0.1", "--port", str(server.port),
     "--token", TOKEN, "--interval", "0.2"],
    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
try:
    deadline = time.time() + 10
    while time.time() < deadline and not server.list_sessions():
        time.sleep(0.05)
    check("cpp agent registers with python server", bool(server.list_sessions()))
    if server.list_sessions():
        cid = server.list_sessions()[0]["client_id"]
        deadline = time.time() + 10
        while time.time() < deadline and server.sessions[cid].last_metrics is None:
            time.sleep(0.05)
        m = server.sessions[cid].last_metrics
        check("python server receives cpp metrics", m is not None)
        if m:
            check("metrics carry cpu/memory/disk fields",
                  all(k in m for k in ("cpu_percent_total", "memory", "disk_root")))
        reply = server.send_query(cid, "ping", timeout=5)
        check("query 'ping' -> pong",
              reply.get("type") == "response" and "pong" in reply.get("payload", {}))
        reply = server.send_query(cid, "get platform_info.system", timeout=5)
        check("get platform_info.system == 'Linux'",
              reply.get("type") == "response" and reply["payload"]["value"] == "Linux")
finally:
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
    server.stop()

# wrong token must be rejected by the python server
server2 = CollectorServer("127.0.0.1", 0, token=TOKEN).start()
bad = subprocess.Popen(
    [str(AGENT_BIN), "--host", "127.0.0.1", "--port", str(server2.port),
     "--token", "wrong", "--interval", "0.2", "--max-retries", "1"],
    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
out, _ = bad.communicate(timeout=15)
time.sleep(0.3)
check("cpp agent with wrong token is denied",
      not server2.list_sessions() and "authentication failed" in out)
server2.stop()

# ---------------------------------------------------------------- Test B
print("--- Test B: raw Python client -> C++ server binary ---")
s = socket.socket()
s.bind(("127.0.0.1", 0))
port = s.getsockname()[1]
s.close()
proc = subprocess.Popen(
    [str(SERVER_BIN), "--host", "127.0.0.1", "--port", str(port),
     "--token", TOKEN, "--mem-threshold", "0"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
try:
    # wrong token -> denied + connection closed
    c = connect_with_retry(port)
    c.settimeout(5)
    c.sendall(json.dumps({"type": "hello", "token": "nope", "hostname": "evil"}).encode() + b"\n")
    msg, _ = recv_msg(c)
    closed = c.recv(1024) == b""
    check("cpp server denies wrong token and closes socket",
          msg == {"type": "denied", "reason": "invalid or missing token"} and closed)
    c.close()

    # correct token -> welcome, then we push one metrics heartbeat
    c = connect_with_retry(port)
    c.settimeout(5)
    c.sendall(json.dumps({"type": "hello", "token": TOKEN, "hostname": "py-agent"}).encode() + b"\n")
    msg, _ = recv_msg(c)
    check("cpp server welcomes python client",
          msg is not None and msg.get("type") == "welcome"
          and msg.get("client_id", "").startswith("py-agent-"))
    metrics = {"type": "metrics", "timestamp": time.time(), "hostname": "py-agent",
               "cpu_percent_total": 5.0, "memory": {"percent": 42.0},
               "disk_root": {"percent": 10.0}}
    c.sendall(json.dumps(metrics).encode() + b"\n")
    time.sleep(0.4)  # let the server store metrics + fire the alert
    proc.stdin.write("list\nalerts\nexit\n")
    proc.stdin.flush()
    out, _ = proc.communicate(timeout=15)
    check("console 'list' shows the python client", "py-agent-" in out)
    check("threshold alert fired in cpp server log", "[alert]" in out and "memory.percent" in out)
    c.close()
except Exception as e:  # noqa: BLE001
    check(f"cpp server test crashed: {e}", False)
    proc.kill()

print()
print(f"{sum(results)}/{len(results)} cross-language checks passed")
sys.exit(0 if all(results) else 1)
