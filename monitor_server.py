#!/usr/bin/env python3
"""
Collector server / dashboard for the advanced monitoring agent.

Accepts multiple agents (monitor_agent.py), authenticates them with a shared
token, stores their metric heartbeats, raises threshold alerts, and provides
an interactive console:

    list                          show connected agents
    query <client_id> <command>   send a read-only command to one agent
    broadcast <command>           send a command to every agent
    metrics <client_id>           show the latest heartbeat of an agent
    history <client_id> [n]       show the last n heartbeat samples
    alerts [clear]                show (or clear) triggered alerts
    exit                          shut the server down

Intended only for machines you own or are explicitly authorized to monitor.
"""

import argparse
import hmac
import os
import secrets
import socket
import ssl
import sys
import threading
import time
from datetime import datetime
from pprint import pprint

from protocol import LineConnection, ProtocolError

SERVER_VERSION = "2.0.0"

# Default alert thresholds; metric paths refer to fields in the agent's
# "metrics" message (see monitor_agent.collect_metrics).
DEFAULT_THRESHOLDS = {
    "cpu_percent_total": 90.0,
    "memory.percent": 90.0,
    "disk_root.percent": 90.0,
}


def check_alerts(metrics, thresholds):
    """Return a list of alerts for any metric at or above its threshold."""
    alerts = []

    def value_at(path):
        cur = metrics
        for part in path.split("."):
            if not isinstance(cur, dict) or part not in cur:
                return None
            cur = cur[part]
        return cur

    for metric, threshold in thresholds.items():
        value = value_at(metric)
        if isinstance(value, (int, float)) and value >= threshold:
            alerts.append({
                "timestamp": time.time(),
                "metric": metric,
                "value": round(float(value), 2),
                "threshold": threshold,
            })
    return alerts


class ClientSession:
    def __init__(self, client_id, conn, address, hello):
        self.client_id = client_id
        self.conn = conn
        self.address = address
        self.hello = hello
        self.registered_at = time.time()
        self.last_seen = time.time()
        self.last_metrics = None
        self.history = []  # list of (timestamp, metrics_message)
        self.lock = threading.Lock()       # guards metrics/history/pending
        self.send_lock = threading.Lock()  # serializes writes to the socket
        self.pending = {}                  # request_id -> {"event", "result"}
        self.alive = True

    def send(self, message):
        with self.send_lock:
            self.conn.send(message)

    def summary(self):
        metrics = self.last_metrics or {}
        memory = (metrics.get("memory") or {}).get("percent")
        return {
            "client_id": self.client_id,
            "address": f"{self.address[0]}:{self.address[1]}",
            "hostname": metrics.get("hostname") or self.hello.get("hostname"),
            "agent_version": metrics.get("agent_version") or self.hello.get("agent_version"),
            "last_seen": datetime.fromtimestamp(self.last_seen).strftime("%H:%M:%S"),
            "cpu_percent": metrics.get("cpu_percent_total"),
            "memory_percent": memory,
            "disk_percent": (metrics.get("disk_root") or {}).get("percent"),
        }


class CollectorServer:
    def __init__(self, host, port, token, ssl_context=None,
                 thresholds=None, max_history=200):
        self.host = host
        self.port = port
        self.token = token
        self.ssl_context = ssl_context
        self.thresholds = dict(thresholds or DEFAULT_THRESHOLDS)
        self.max_history = max_history
        self.sessions = {}
        self.alerts = []
        self._lock = threading.Lock()   # guards sessions/alerts
        self._print_lock = threading.Lock()
        self._stop = threading.Event()
        self._sock = None
        self._accept_thread = None

    def log(self, message):
        with self._print_lock:
            print(message, flush=True)

    # ------------------------------------------------------------------ lifecycle
    def start(self):
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind((self.host, self.port))
        self._sock.listen(16)
        self.port = self._sock.getsockname()[1]  # resolve ephemeral port 0
        self._accept_thread = threading.Thread(target=self._accept_loop, daemon=True)
        self._accept_thread.start()
        return self

    def stop(self):
        self._stop.set()
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
        with self._lock:
            sessions = list(self.sessions.values())
        for session in sessions:
            session.alive = False
            try:
                session.conn.sock.close()
            except OSError:
                pass
        if self._accept_thread is not None:
            self._accept_thread.join(timeout=2)

    def _accept_loop(self):
        while not self._stop.is_set():
            try:
                raw, addr = self._sock.accept()
            except OSError:
                break
            threading.Thread(target=self._handle_client, args=(raw, addr),
                             daemon=True).start()

    # ------------------------------------------------------------------ per-client
    def _handle_client(self, raw, addr):
        session = None
        try:
            if self.ssl_context is not None:
                raw = self.ssl_context.wrap_socket(raw, server_side=True)
            conn = LineConnection(raw)
            hello = conn.recv()
            if (not isinstance(hello, dict)
                    or hello.get("type") != "hello"
                    or not hmac.compare_digest(str(hello.get("token", "")), self.token)):
                try:
                    conn.send({"type": "denied", "reason": "invalid or missing token"})
                except OSError:
                    pass
                self.log(f"[server] rejected unauthenticated peer {addr[0]}:{addr[1]}")
                return
            client_id = f"{hello.get('hostname') or addr[0]}-{secrets.token_hex(3)}"
            session = ClientSession(client_id, conn, addr, hello)
            with self._lock:
                self.sessions[client_id] = session
            session.send({"type": "welcome", "client_id": client_id,
                          "server_version": SERVER_VERSION})
            self.log(f"[server] client connected: {client_id} from {addr[0]}:{addr[1]}")
            self._message_loop(session)
        except (OSError, ProtocolError) as exc:
            self.log(f"[server] connection error from {addr[0]}:{addr[1]}: {exc}")
        finally:
            if session is not None:
                session.alive = False
                with self._lock:
                    self.sessions.pop(session.client_id, None)
                with session.lock:
                    for entry in session.pending.values():
                        entry["event"].set()  # wake up anyone waiting on a query
                try:
                    raw.close()
                except OSError:
                    pass
                self.log(f"[server] client disconnected: {session.client_id}")

    def _message_loop(self, session):
        while not self._stop.is_set():
            message = session.conn.recv()
            if message is None:
                return
            session.last_seen = time.time()
            mtype = message.get("type")
            if mtype == "metrics":
                self._store_metrics(session, message)
            elif mtype in ("response", "error"):
                self._route_response(session, message)

    def _store_metrics(self, session, message):
        with session.lock:
            session.last_metrics = message
            session.history.append((message.get("timestamp", time.time()), message))
            if len(session.history) > self.max_history:
                session.history = session.history[-self.max_history:]
        for alert in check_alerts(message, self.thresholds):
            alert["client_id"] = session.client_id
            with self._lock:
                self.alerts.append(alert)
            self.log(f"[alert] {session.client_id}: {alert['metric']} = "
                     f"{alert['value']}% (threshold {alert['threshold']}%)")

    def _route_response(self, session, message):
        request_id = message.get("request_id")
        with session.lock:
            entry = session.pending.pop(request_id, None)
        if entry is not None:
            entry["result"] = message
            entry["event"].set()

    # ------------------------------------------------------------------ queries
    def send_query(self, client_id, command, timeout=10.0):
        """Send a read-only command to one agent and wait for its reply."""
        with self._lock:
            session = self.sessions.get(client_id)
        if session is None or not session.alive:
            raise KeyError(f"unknown or disconnected client: {client_id}")
        request_id = secrets.token_hex(8)
        entry = {"event": threading.Event(), "result": None}
        with session.lock:
            session.pending[request_id] = entry
        try:
            session.send({"type": "command", "request_id": request_id,
                          "command": command})
        except OSError:
            with session.lock:
                session.pending.pop(request_id, None)
            raise ConnectionError(f"failed to reach {client_id}")
        if not entry["event"].wait(timeout):
            with session.lock:
                session.pending.pop(request_id, None)
            raise TimeoutError(f"no response from {client_id} within {timeout}s")
        return entry["result"]

    def broadcast(self, command, timeout=10.0):
        """Send a command to every connected agent; returns {client_id: reply}."""
        with self._lock:
            client_ids = [s.client_id for s in self.sessions.values() if s.alive]
        results = {}
        for client_id in client_ids:
            try:
                results[client_id] = self.send_query(client_id, command, timeout)
            except (KeyError, TimeoutError, ConnectionError) as exc:
                results[client_id] = {"type": "error", "message": str(exc)}
        return results

    def list_sessions(self):
        with self._lock:
            return [s.summary() for s in self.sessions.values()]


def make_server_tls_context(certfile, keyfile):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile, keyfile)
    return ctx


# ---------------------------------------------------------------------- console
HELP_TEXT = """Commands:
  list                          show connected agents
  query <client_id> <command>   send a read-only command to one agent
                                (try: ping | get_system_info | get memory_info.memory_percent)
  broadcast <command>           send a command to every connected agent
  metrics <client_id>           show the agent's latest heartbeat
  history <client_id> [n]       show the last n heartbeat samples (default 10)
  alerts [clear]                show triggered threshold alerts (or clear them)
  help                          show this help
  exit                          shut the server down"""


def console_loop(server):
    print(HELP_TEXT)
    while not server._stop.is_set():
        try:
            line = input("monitor> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            server.stop()
            break
        if not line:
            continue
        parts = line.split()
        cmd = parts[0].lower()
        try:
            if cmd in ("exit", "quit"):
                server.stop()
                break
            elif cmd == "help":
                print(HELP_TEXT)
            elif cmd == "list":
                sessions = server.list_sessions()
                if not sessions:
                    print("no agents connected")
                for s in sessions:
                    print(f"  {s['client_id']:<24} {s['address']:<22} "
                          f"cpu={s['cpu_percent']} mem={s['memory_percent']}% "
                          f"disk={s['disk_percent']}% last_seen={s['last_seen']}")
            elif cmd == "query" and len(parts) >= 3:
                client_id = parts[1]
                command = line.split(None, 2)[2]
                reply = server.send_query(client_id, command)
                if reply.get("type") == "response":
                    pprint(reply.get("payload"))
                else:
                    print(reply.get("message", reply))
            elif cmd == "broadcast" and len(parts) >= 2:
                command = line.split(None, 1)[1]
                for client_id, reply in server.broadcast(command).items():
                    print(f"--- {client_id} ---")
                    pprint(reply.get("payload", reply.get("message", reply)))
            elif cmd == "metrics" and len(parts) >= 2:
                with server._lock:
                    session = server.sessions.get(parts[1])
                if session is None:
                    print(f"unknown client: {parts[1]}")
                else:
                    pprint(session.last_metrics)
            elif cmd == "history" and len(parts) >= 2:
                with server._lock:
                    session = server.sessions.get(parts[1])
                if session is None:
                    print(f"unknown client: {parts[1]}")
                else:
                    n = int(parts[2]) if len(parts) > 2 else 10
                    with session.lock:
                        samples = session.history[-n:]
                    for ts, m in samples:
                        print(f"  {datetime.fromtimestamp(ts).strftime('%H:%M:%S')} "
                              f"cpu={m.get('cpu_percent_total')}% "
                              f"mem={ (m.get('memory') or {}).get('percent') }% "
                              f"disk={ (m.get('disk_root') or {}).get('percent') }%")
            elif cmd == "alerts":
                if len(parts) > 1 and parts[1].lower() == "clear":
                    with server._lock:
                        server.alerts.clear()
                    print("alerts cleared")
                else:
                    with server._lock:
                        alerts = list(server.alerts)
                    if not alerts:
                        print("no alerts triggered")
                    for a in alerts:
                        print(f"  {datetime.fromtimestamp(a['timestamp']).strftime('%H:%M:%S')} "
                              f"{a['client_id']}: {a['metric']} = {a['value']}% "
                              f"(threshold {a['threshold']}%)")
            else:
                print("unknown command — type 'help'")
        except (KeyError, TimeoutError, ConnectionError, ValueError) as exc:
            print(f"error: {exc}")


def build_arg_parser():
    parser = argparse.ArgumentParser(
        description="Collector server/dashboard — authorized systems only")
    parser.add_argument("--host", default="127.0.0.1", help="interface to bind")
    parser.add_argument("--port", type=int, default=5000, help="port to listen on")
    parser.add_argument("--token", default=os.environ.get("MONITOR_TOKEN"),
                        help="shared token agents must present (or set MONITOR_TOKEN)")
    parser.add_argument("--tls", action="store_true", help="require TLS for agent connections")
    parser.add_argument("--certfile", default=None, help="TLS certificate (with --tls)")
    parser.add_argument("--keyfile", default=None, help="TLS private key (with --tls)")
    parser.add_argument("--cpu-threshold", type=float, default=DEFAULT_THRESHOLDS["cpu_percent_total"])
    parser.add_argument("--mem-threshold", type=float, default=DEFAULT_THRESHOLDS["memory.percent"])
    parser.add_argument("--disk-threshold", type=float, default=DEFAULT_THRESHOLDS["disk_root.percent"])
    return parser


def main():
    args = build_arg_parser().parse_args()
    token = args.token
    if not token:
        token = secrets.token_hex(16)
        print(f"warning: no --token / MONITOR_TOKEN set; generated an ephemeral token:\n"
              f"  {token}\nstart agents with: MONITOR_TOKEN={token} python monitor_agent.py",
              file=sys.stderr)
    if args.tls and not (args.certfile and args.keyfile):
        print("error: --tls requires --certfile and --keyfile "
              "(e.g. openssl req -x509 -newkey rsa:2048 -nodes "
              "-keyout key.pem -out cert.pem -days 365 -subj '/CN=localhost')",
              file=sys.stderr)
        sys.exit(2)
    ssl_context = (make_server_tls_context(args.certfile, args.keyfile)
                   if args.tls else None)
    thresholds = {
        "cpu_percent_total": args.cpu_threshold,
        "memory.percent": args.mem_threshold,
        "disk_root.percent": args.disk_threshold,
    }
    server = CollectorServer(args.host, args.port, token,
                             ssl_context=ssl_context, thresholds=thresholds)
    server.start()
    print(f"Collector server v{SERVER_VERSION} listening on {args.host}:{server.port}"
          f"{' (TLS)' if args.tls else ''}")
    try:
        console_loop(server)
    except KeyboardInterrupt:
        server.stop()
    print("server shut down")


if __name__ == "__main__":
    main()
