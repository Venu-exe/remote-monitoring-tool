#!/usr/bin/env python3
"""
Shared newline-delimited JSON protocol used by monitor_agent.py and
monitor_server.py.

Every message is a single JSON object terminated by b"\\n" on a TCP
(optionally TLS) stream. Messages are dicts with a "type" field, e.g.:

    {"type": "hello", "token": "...", "hostname": "..."}
    {"type": "metrics", "timestamp": 1234.5, ...}
    {"type": "command", "request_id": "abc", "command": "get memory_info.memory_percent"}
    {"type": "response", "request_id": "abc", "payload": ...}
"""

import json

# Cap a single message at 4 MiB so a peer cannot exhaust memory by
# streaming an endless line.
MAX_MESSAGE_BYTES = 4 * 1024 * 1024


class ProtocolError(Exception):
    """Raised when a peer sends malformed or oversized data."""


class LineConnection:
    """Wraps a socket with a newline-delimited JSON message protocol."""

    def __init__(self, sock):
        self.sock = sock
        self._buffer = b""

    def send(self, message):
        if not isinstance(message, dict):
            raise ProtocolError("messages must be JSON objects (dicts)")
        payload = json.dumps(message, default=str).encode("utf-8")
        if len(payload) > MAX_MESSAGE_BYTES:
            raise ProtocolError("outgoing message exceeds size limit")
        self.sock.sendall(payload + b"\n")

    def recv(self):
        """Return the next decoded message, or None if the peer closed.

        May raise socket.timeout if the socket has a timeout set.
        """
        while True:
            if b"\n" in self._buffer:
                line, self._buffer = self._buffer.split(b"\n", 1)
                if not line.strip():
                    continue
                try:
                    message = json.loads(line.decode("utf-8", errors="replace"))
                except json.JSONDecodeError as exc:
                    raise ProtocolError(f"invalid JSON from peer: {exc}") from exc
                if not isinstance(message, dict):
                    raise ProtocolError("message must be a JSON object")
                return message
            chunk = self.sock.recv(65536)
            if not chunk:
                return None
            self._buffer += chunk
            if len(self._buffer) > MAX_MESSAGE_BYTES:
                raise ProtocolError("incoming message exceeds size limit")
