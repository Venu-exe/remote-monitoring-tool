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
