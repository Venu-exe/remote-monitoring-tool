#!/usr/bin/env python3
"""
Advanced monitoring agent (learning / telemetry tool).

Connects to a collector server (monitor_server.py), authenticates with a
shared token, streams system metrics on a heartbeat interval, and answers
read-only query commands such as:

    ping
    get_system_info
    get <dot.path>        e.g. get memory_info.memory_percent
    exit

Intended only for machines you own or are explicitly authorized to monitor.
The agent exposes metrics only -- it has no remote command execution,
file access, or persistence features.
"""

import argparse
import os
import platform
import random
import socket
import ssl
import sys
import threading
import time

import psutil

from protocol import LineConnection, ProtocolError

AGENT_VERSION = "2.0.0"
RECV_POLL_SECONDS = 0.2  # short timeout so the heartbeat loop stays responsive


class AuthError(Exception):
    """The server rejected our handshake token."""


class CloseRequested(Exception):
    """The server asked the agent to disconnect."""


def get_by_path(obj, path):
    """Fetch a nested value using a dot-separated path, e.g. 'memory_info.percent'."""
    cur = obj
    for part in path.split("."):
        if isinstance(cur, dict) and part in cur:
            cur = cur[part]
        else:
            raise KeyError(part)
    return cur


def top_processes(limit=5):
    """Top processes by memory usage (read-only inventory for monitoring)."""
    procs = []
    for proc in psutil.process_iter(["pid", "name", "memory_percent"]):
        try:
            procs.append({
                "pid": proc.info["pid"],
                "name": proc.info.get("name"),
                "memory_percent": round(proc.info.get("memory_percent") or 0.0, 2),
            })
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            continue
    procs.sort(key=lambda p: p["memory_percent"], reverse=True)
    return procs[:limit]


def collect_metrics():
    """One heartbeat sample of system metrics."""
    vm = psutil.virtual_memory()
    disk = psutil.disk_usage("/")
    net = psutil.net_io_counters()
    try:
        load_avg = list(os.getloadavg())
    except (AttributeError, OSError):
        load_avg = None  # e.g. Windows
    # First percpu call primes the counters; the second (0.1s) is meaningful.
    percpu = psutil.cpu_percent(interval=None, percpu=True)
    total = psutil.cpu_percent(interval=0.1)
    return {
        "type": "metrics",
        "timestamp": time.time(),
        "hostname": platform.node(),
        "agent_version": AGENT_VERSION,
        "uptime_seconds": round(time.time() - psutil.boot_time(), 1),
        "cpu_percent_total": total,
        "cpu_percent_per_core": percpu,
        "load_avg": load_avg,
        "memory": vm._asdict(),
        "disk_root": disk._asdict(),
        "net_io": net._asdict(),
        "process_count": len(psutil.pids()),
        "top_processes": top_processes(5),
    }


def full_system_info():
    """Detailed read-only system inventory, used by 'get_system_info' / 'get <path>'."""
    vm = psutil.virtual_memory()
    swap = psutil.swap_memory()
    freq = psutil.cpu_freq()
    net = psutil.net_io_counters()
    try:
        conns = [c._asdict() for c in psutil.net_connections()][:100]
    except (psutil.AccessDenied, OSError):
        conns = []
    return {
        "platform_info": {
            "system": platform.system(),
            "node": platform.node(),
            "release": platform.release(),
            "version": platform.version(),
            "machine": platform.machine(),
            "processor": platform.processor(),
            "boot_time": time.ctime(psutil.boot_time()),
            "python_version": platform.python_version(),
        },
        "cpu_info": {
            "physical_cores": psutil.cpu_count(logical=False),
            "logical_cores": psutil.cpu_count(logical=True),
            "cpu_freq": freq._asdict() if freq else {},
            "cpu_times": psutil.cpu_times()._asdict(),
        },
        "memory_info": {
            "virtual_memory": vm._asdict(),
            "swap_memory": swap._asdict(),
        },
        "disk_info": {
            "partitions": [p._asdict() for p in psutil.disk_partitions()],
            "usage_root": psutil.disk_usage("/")._asdict(),
            "io_counters": psutil.disk_io_counters()._asdict() if psutil.disk_io_counters() else {},
        },
        "network_info": {
            "io_counters": net._asdict(),
            "connection_count": len(conns),
            "connections": conns,
            "interfaces": {
                iface: [a._asdict() for a in addrs]
                for iface, addrs in psutil.net_if_addrs().items()
            },
        },
    }


def make_client_tls_context(cafile=None):
    ctx = ssl.create_default_context(cafile=cafile)
    if cafile is None:
        # Lab convenience: without a CA bundle we cannot verify the server.
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
    return ctx


class Agent:
    def __init__(self, host, port, token, interval=5.0, ssl_context=None, max_retries=None):
        self.host = host
        self.port = port
        self.token = token
        self.interval = interval
        self.ssl_context = ssl_context
        self.max_retries = max_retries

    def _connect(self):
        raw = socket.create_connection((self.host, self.port), timeout=10)
        if self.ssl_context is not None:
            raw = self.ssl_context.wrap_socket(raw, server_hostname=self.host)
        raw.settimeout(RECV_POLL_SECONDS)
        conn = LineConnection(raw)
        conn.send({
            "type": "hello",
            "token": self.token,
            "hostname": platform.node(),
            "agent_version": AGENT_VERSION,
        })
        reply = conn.recv()
        if reply is None:
            raise ConnectionError("server closed the connection during handshake")
        if reply.get("type") == "denied":
            raise AuthError(reply.get("reason", "access denied"))
        if reply.get("type") != "welcome":
            raise ProtocolError(f"unexpected handshake reply: {reply}")
        return conn, reply.get("client_id")

    def _handle_command(self, conn, message):
        command = str(message.get("command", ""))
        request_id = message.get("request_id")
        cmd = command.strip().lower()
        if cmd in ("exit", "bye"):
            raise CloseRequested()
        if cmd == "ping":
            payload = {"pong": time.time()}
        elif cmd == "get_system_info":
            payload = full_system_info()
        elif cmd.startswith("get "):
            path = command[4:].strip()
            try:
                payload = {"path": path, "value": get_by_path(full_system_info(), path)}
            except KeyError:
                conn.send({"type": "error", "request_id": request_id,
                           "message": f"path not found: {path}"})
                return
        else:
            conn.send({"type": "error", "request_id": request_id,
                       "message": f"unknown command: {command}"})
            return
        conn.send({"type": "response", "request_id": request_id, "payload": payload})

    def _session(self, conn, stop_event):
        """Send heartbeats and answer commands until the link drops or we're stopped."""
        next_report = 0.0
        while not stop_event.is_set():
            now = time.monotonic()
            if now >= next_report:
                conn.send(collect_metrics())
                next_report = now + self.interval
            try:
                message = conn.recv()
            except socket.timeout:
                continue
            if message is None:
                raise ConnectionError("server closed the connection")
            mtype = message.get("type")
            if mtype == "command":
                self._handle_command(conn, message)
            elif mtype == "ping":
                conn.send({"type": "pong", "timestamp": time.time()})
            # unknown message types are ignored

    def run(self, stop_event=None):
        stop_event = stop_event if stop_event is not None else threading.Event()
        attempt = 0
        while not stop_event.is_set():
            conn = None
            try:
                conn, client_id = self._connect()
                attempt = 0
                print(f"[agent] connected to {self.host}:{self.port} as '{client_id}'")
                self._session(conn, stop_event)
            except AuthError as exc:
                print(f"[agent] authentication failed: {exc} (check the shared token)")
                return
            except CloseRequested:
                print("[agent] server asked us to disconnect")
                return
            except (OSError, ProtocolError, ConnectionError) as exc:
                attempt += 1
                if self.max_retries is not None and attempt > self.max_retries:
                    print(f"[agent] giving up after {attempt} attempts: {exc}")
                    return
                wait = min(2 ** (attempt - 1), 30) + random.uniform(0, 0.5)
                print(f"[agent] connection problem ({exc}); reconnecting in {wait:.1f}s")
                stop_event.wait(wait)  # interruptible sleep
            finally:
                if conn is not None:
                    try:
                        conn.sock.close()
                    except OSError:
                        pass


def build_arg_parser():
    parser = argparse.ArgumentParser(
        description="Advanced monitoring agent — authorized systems only")
    parser.add_argument("--host", default="127.0.0.1", help="collector server host")
    parser.add_argument("--port", type=int, default=5000, help="collector server port")
    parser.add_argument("--token", default=os.environ.get("MONITOR_TOKEN"),
                        help="shared token (or set the MONITOR_TOKEN env var)")
    parser.add_argument("--interval", type=float, default=5.0,
                        help="seconds between metric heartbeats (default: 5)")
    parser.add_argument("--tls", action="store_true",
                        help="encrypt the connection with TLS (server must use TLS too)")
    parser.add_argument("--cafile", default=None,
                        help="CA bundle to verify the server certificate (with --tls)")
    parser.add_argument("--max-retries", type=int, default=None,
                        help="give up after N failed connection attempts (default: never)")
    return parser


def main():
    args = build_arg_parser().parse_args()
    if not args.token:
        print("error: provide --token or set the MONITOR_TOKEN environment variable",
              file=sys.stderr)
        sys.exit(2)
    ssl_context = make_client_tls_context(args.cafile) if args.tls else None
    if args.tls and args.cafile is None:
        print("[agent] warning: --tls without --cafile disables certificate verification "
              "(acceptable for local labs only)", file=sys.stderr)
    agent = Agent(args.host, args.port, args.token, interval=args.interval,
                  ssl_context=ssl_context, max_retries=args.max_retries)
    try:
        agent.run()
    except KeyboardInterrupt:
        print("\n[agent] stopped by user")


if __name__ == "__main__":
    main()
