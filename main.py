#!/usr/bin/env python3

# build a simple remote monitoring client for learning purposes

import json
import socket
import time
import platform
import locale

try:
    import psutil
except ImportError:
    psutil = None

DEFAULT_SERVER_IP = "127.0.0.1"
DEFAULT_SERVER_PORT = 5000
RETRY_MAX = 5
RETRY_BASE_SECONDS = 1


def get_system_info():
    if psutil is None:
        return {"error": "psutil is not installed. Install it with 'pip install psutil'"}

    info = {}
    platform_info = {
        "system": platform.system(),
        "os_version": platform.version(),
        "os_release": platform.release(),
        "machine": platform.machine(),
        "os_build": platform.architecture()[0],
        "system_boot_time": time.ctime(psutil.boot_time()),
        "process_count": len(psutil.pids()),
        "system_locale": locale.getdefaultlocale(),
        "system_manufacturer": platform.uname().system,
    }
    info["platform_info"] = platform_info

    cpu_info = {
        "cpu_count": psutil.cpu_count(logical=True),
        # non-blocking: use 0.0 for immediate sample (no 0.5s wait)
        "cpu_percent": psutil.cpu_percent(interval=0.0),
        "cpu_freq": psutil.cpu_freq()._asdict() if psutil.cpu_freq() else {},
        "cpu_time": psutil.cpu_times()._asdict(),
    }
    info["cpu_info"] = cpu_info

    virtual_memory = psutil.virtual_memory()
    swap_memory = psutil.swap_memory()
    memory_info = {
        "virtual_memory": virtual_memory._asdict(),
        "swap_memory": swap_memory._asdict(),
        "total_memory": virtual_memory.total,
        "available_memory": virtual_memory.available,
        "used_memory": virtual_memory.used,
        "memory_percent": virtual_memory.percent,
    }
    info["memory_info"] = memory_info

    disk_info = {
        "disk_partitions": [partition._asdict() for partition in psutil.disk_partitions()],
        "disk_usage": {partition.mountpoint: psutil.disk_usage(partition.mountpoint)._asdict() for partition in psutil.disk_partitions()},
        "disk_io_counters": psutil.disk_io_counters()._asdict(),
    }
    info["disk_info"] = disk_info

    net_io = psutil.net_io_counters()
    network_info = {
        "network_io_counters": net_io._asdict(),
        "network_connections": [conn._asdict() for conn in psutil.net_connections()],
        "network_interfaces": {iface: [addr._asdict() for addr in addrs] for iface, addrs in psutil.net_if_addrs().items()},
        "network_stats": {iface: stats._asdict() for iface, stats in psutil.net_if_stats().items()},
    }
    info["network_info"] = network_info

    locale_info = {
        "default_locale": locale.getdefaultlocale(),
        "preferred_encoding": locale.getpreferredencoding(),
    }
    info["locale_info"] = locale_info

    return info


def send_message(sock, data):
    message = json.dumps(data)
    sock.sendall((message + "\n").encode("utf-8"))


def recv_message(sock):
    buffer = b""
    while True:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            # allow periodic checks and responsive KeyboardInterrupt
            continue
        if not chunk:
            return None
        buffer += chunk
        if b"\n" in buffer:
            line, buffer = buffer.split(b"\n", 1)
            try:
                return json.loads(line.decode("utf-8", errors="replace"))
            except json.JSONDecodeError:
                return None


def connect_with_retry(server_ip, server_port):
    attempt = 0
    while attempt < RETRY_MAX:
        try:
            client = socket.create_connection((server_ip, server_port), timeout=10)
            # set a short recv timeout so recv calls are interruptible
            client.settimeout(1.0)
            return client
        except OSError as exc:
            attempt += 1
            wait_time = RETRY_BASE_SECONDS * (2 ** (attempt - 1))
            print(f"Connection attempt {attempt}/{RETRY_MAX} failed: {exc}. Retrying in {wait_time}s...")
            # sleep in small increments so Ctrl+C interrupts quickly
            slept = 0.0
            try:
                while slept < wait_time:
                    time.sleep(0.1)
                    slept += 0.1
            except KeyboardInterrupt:
                raise
    raise ConnectionError(f"Unable to connect to {server_ip}:{server_port}")


def start_client(server_ip, server_port):
    client = connect_with_retry(server_ip, server_port)
    print(f"Connected to server {server_ip}:{server_port}")

    try:
        while True:
            message = recv_message(client)
            if message is None:
                print("Server closed the connection.")
                break

            command = message.get("command") if isinstance(message, dict) else str(message)
            if not isinstance(command, str):
                send_message(client, {"error": "Invalid command format"})
                continue

            # support server requests for a single field using: get <dot.path>
            def get_by_path(obj, path):
                parts = path.split('.') if path else []
                cur = obj
                for p in parts:
                    if isinstance(cur, dict) and p in cur:
                        cur = cur[p]
                    else:
                        raise KeyError(p)
                return cur

            if command.lower() == "exit":
                send_message(client, {"status": "closing"})
                break
            elif command.lower().startswith("get "):
                path = command[4:].strip()
                if not path:
                    send_message(client, {"error": "No path provided for get command"})
                    continue
                system_info = get_system_info()
                try:
                    value = get_by_path(system_info, path)
                    send_message(client, {"response": {"path": path, "value": value}})
                except KeyError:
                    send_message(client, {"error": f"Path not found: {path}"})
                continue
            elif command.lower() == "get_system_info":
                system_info = get_system_info()
                send_message(client, {"response": system_info})
            else:
                send_message(client, {"error": f"Unknown command: {command}"})
    except Exception as exc:
        print(f"Error: {exc}")
    finally:
        client.close()
        print("Connection closed.")


def prompt_for_server_settings():
    server_ip = input(f"Enter server IP [{DEFAULT_SERVER_IP}]: ").strip() or DEFAULT_SERVER_IP
    port_text = input(f"Enter server port [{DEFAULT_SERVER_PORT}]: ").strip() or str(DEFAULT_SERVER_PORT)
    return server_ip, int(port_text)


if __name__ == "__main__":
    server_ip, server_port = prompt_for_server_settings()
    start_client(server_ip, server_port)



