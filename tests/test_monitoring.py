"""Tests for the advanced monitoring suite (protocol + end-to-end agent/server)."""

import platform
import socket
import threading
import time

import pytest

from protocol import LineConnection, ProtocolError
from monitor_agent import Agent, get_by_path
from monitor_server import CollectorServer, check_alerts


# --------------------------------------------------------------------- protocol
def test_line_connection_roundtrip():
    a, b = socket.socketpair()
    try:
        ca, cb = LineConnection(a), LineConnection(b)
        ca.send({"type": "hello", "n": 42, "nested": {"x": [1, 2, 3]}})
        ca.send({"type": "second"})
        assert cb.recv() == {"type": "hello", "n": 42, "nested": {"x": [1, 2, 3]}}
        assert cb.recv() == {"type": "second"}
        a.close()
        assert cb.recv() is None  # peer closed -> None
    finally:
        a.close()
        b.close()


def test_line_connection_rejects_non_dict():
    a, b = socket.socketpair()
    try:
        ca = LineConnection(a)
        with pytest.raises(ProtocolError):
            ca.send(["not", "a", "dict"])
    finally:
        a.close()
        b.close()


def test_get_by_path():
    obj = {"a": {"b": {"c": 7}}}
    assert get_by_path(obj, "a.b.c") == 7
    with pytest.raises(KeyError):
        get_by_path(obj, "a.b.missing")


def test_check_alerts():
    metrics = {
        "cpu_percent_total": 95.0,
        "memory": {"percent": 50.0},
        "disk_root": {"percent": 97.2},
    }
    thresholds = {
        "cpu_percent_total": 90.0,
        "memory.percent": 90.0,
        "disk_root.percent": 90.0,
    }
    alerts = check_alerts(metrics, thresholds)
    assert {a["metric"] for a in alerts} == {"cpu_percent_total", "disk_root.percent"}
    assert check_alerts({"cpu_percent_total": 10.0}, thresholds) == []


# ------------------------------------------------------------------ end-to-end
@pytest.fixture
def server():
    srv = CollectorServer("127.0.0.1", 0, token="test-token",
                          thresholds={"memory.percent": 0.0})  # always alerts
    srv.start()
    yield srv
    srv.stop()


def _wait_for(predicate, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(0.05)
    return False


def test_agent_registers_streams_metrics_and_answers_queries(server):
    stop = threading.Event()
    agent = Agent("127.0.0.1", server.port, token="test-token", interval=0.2)
    thread = threading.Thread(target=agent.run, args=(stop,), daemon=True)
    thread.start()
    try:
        assert _wait_for(lambda: server.list_sessions()), "agent never registered"
        session_id = server.list_sessions()[0]["client_id"]
        assert _wait_for(lambda: server.sessions[session_id].last_metrics is not None), \
            "no metrics received"

        # read-only query: ping
        reply = server.send_query(session_id, "ping", timeout=5)
        assert reply["type"] == "response"
        assert "pong" in reply["payload"]

        # read-only query: dot-path lookup
        reply = server.send_query(session_id, "get platform_info.system", timeout=5)
        assert reply["type"] == "response"
        assert reply["payload"]["value"] == platform.system()

        # unknown path returns a clean error, not a crash
        reply = server.send_query(session_id, "get nope.nothing", timeout=5)
        assert reply["type"] == "error"

        # heartbeat history grows over time
        time.sleep(0.6)
        assert len(server.sessions[session_id].history) >= 2

        # threshold of 0.0 means every heartbeat triggers an alert
        assert _wait_for(lambda: len(server.alerts) > 0), "no alerts recorded"
    finally:
        stop.set()
        thread.join(timeout=5)


def test_wrong_token_is_rejected(server):
    stop = threading.Event()
    agent = Agent("127.0.0.1", server.port, token="wrong-token", interval=0.2,
                  max_retries=1)
    agent.run(stop)  # AuthError -> returns immediately
    time.sleep(0.3)
    assert server.list_sessions() == []


# ------------------------------------------------------------ unauthenticated / hostile peers
def _raw_client(port):
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    sock.settimeout(5)
    return LineConnection(sock)


def test_correct_token_gets_welcome(server):
    conn = _raw_client(server.port)
    conn.send({"type": "hello", "token": "test-token", "hostname": "legit"})
    reply = conn.recv()
    assert reply["type"] == "welcome"
    assert reply["client_id"].startswith("legit-")
    conn.sock.close()


def test_missing_token_denied_and_socket_closed(server):
    conn = _raw_client(server.port)
    conn.send({"type": "hello", "hostname": "intruder"})  # no token field at all
    reply = conn.recv()
    assert reply == {"type": "denied", "reason": "invalid or missing token"}
    # Regression: the server MUST close the socket after denying, otherwise
    # every rejected handshake leaks a file descriptor.
    assert conn.recv() is None
    conn.sock.close()
    assert server.list_sessions() == []


def test_malformed_handshake_dropped(server):
    sock = socket.create_connection(("127.0.0.1", server.port), timeout=5)
    sock.settimeout(5)
    sock.sendall(b"this is not json\n")
    assert sock.recv(4096) == b""  # closed without a reply
    sock.close()
    assert server.list_sessions() == []


def test_non_dict_json_dropped(server):
    sock = socket.create_connection(("127.0.0.1", server.port), timeout=5)
    sock.settimeout(5)
    sock.sendall(b"[1, 2, 3]\n")
    assert sock.recv(4096) == b""
    sock.close()
    assert server.list_sessions() == []


def test_oversized_handshake_dropped(server):
    sock = socket.create_connection(("127.0.0.1", server.port), timeout=5)
    sock.settimeout(5)
    payload = b'{"type":"hello","pad":"' + b"A" * (5 * 1024 * 1024) + b'"}'
    try:
        sock.sendall(payload)
    except OSError:
        pass  # server may close early once the size cap trips
    try:
        assert sock.recv(4096) == b""  # clean EOF: server dropped us
    except ConnectionResetError:
        pass  # RST because the server closed while we were still sending — also fine
    sock.close()
    assert server.list_sessions() == []


def test_broadcast_reaches_all_agents(server):
    stop = threading.Event()
    agents = [Agent("127.0.0.1", server.port, token="test-token", interval=0.5)
              for _ in range(2)]
    threads = [threading.Thread(target=a.run, args=(stop,), daemon=True) for a in agents]
    for t in threads:
        t.start()
    try:
        assert _wait_for(lambda: len(server.list_sessions()) == 2)
        results = server.broadcast("ping", timeout=5)
        assert len(results) == 2
        for reply in results.values():
            assert reply["type"] == "response"
            assert "pong" in reply["payload"]
    finally:
        stop.set()
        for t in threads:
            t.join(timeout=5)
