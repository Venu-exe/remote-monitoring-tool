#!/usr/bin/env python3

# for learning purpose only

import json
import socket
import threading
from pprint import pprint

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 5000


def send_message(sock, data):
    message = json.dumps(data)
    sock.sendall((message + "\n").encode("utf-8"))


def recv_message(sock):
    buffer = b""
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            return None
        buffer += chunk
        if b"\n" in buffer:
            line, buffer = buffer.split(b"\n", 1)
            try:
                return json.loads(line.decode("utf-8", errors="replace"))
            except json.JSONDecodeError:
                return None


def handle_client(client_socket):
    host, port = client_socket.getpeername()
    try:
        while True:
            command = input(f"Enter command to send to {host}:{port} (or 'exit' to close connection): ")
            send_message(client_socket, {"command": command})

            if command.lower() == "exit":
                break

            response = recv_message(client_socket)
            if response is None:
                print(f"Connection to {host}:{port} closed by client.")
                break

            if "response" in response:
                pprint(response["response"])
            elif "error" in response:
                print(response["error"])
            else:
                print(f"Unexpected response: {response}")
    except KeyboardInterrupt:
        print("Server shutting down.")
    finally:
        client_socket.close()
        print(f"Connection to {host}:{port} closed.")


def start_server(host, port):
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((host, port))
    server.listen(5)
    print(f"Server listening on {host}:{port}")

    try:
        while True:
            client_socket, addr = server.accept()
            print(f"Accepted connection from {addr[0]}:{addr[1]}")
            client_handler = threading.Thread(target=handle_client, args=(client_socket,))
            client_handler.start()
    except KeyboardInterrupt:
        print("Server shutting down.")
    finally:
        server.close()


if __name__ == "__main__":
    host = input(f"Enter host IP to bind the server [{DEFAULT_HOST}]: ").strip() or DEFAULT_HOST
    port_text = input(f"Enter port to bind the server [{DEFAULT_PORT}]: ").strip() or str(DEFAULT_PORT)
    port = int(port_text)
    start_server(host, port)



