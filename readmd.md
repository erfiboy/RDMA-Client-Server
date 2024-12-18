# RDMA Client-Server Example

This project implements an RDMA (Remote Direct Memory Access) client-server application. It supports command-line arguments for customization, such as specifying ports, buffer sizes, and IP addresses, with additional features like data validation and memory initialization.

---

## Usage
### Server
```bash
Usage: ./server -d <dst-port> -s <buffer-size> -p <source-port> -i <src-ip> -r <remote-ip> -v <validate> -f <fill memory with random byte> -h <help>
```

### client
```bash
Usage: ./client -p <dst-port> -s <buffer-size> -i <dst-ip> -v <validate> -h <help>
```

## Arguments

### Common Options
- **`-p <dst-port>`**: Specifies the destination port to connect to.
- **`-s <buffer-size>`**: Sets the buffer size in bytes.
- **`-i <dst-ip>`**: Specifies the destination IP address.
- **`-v <validate>`**: Enables data validation (1 for true, 0 for false).
- **`-h <help>`**: Displays the usage information.

### Server-Specific Options
- **`-d <dst-port>`**: Specifies the listening port for incoming connections.
- **`-r <remote-ip>`**: Sets the remote IP address for RDMA communication.
- **`-f <fill memory with random byte>`**: If set, fills the memory region with random bytes.

## Examples
### Running the Server
```bash
./server -p 23456 -s 104857600 -i "192.168.5.6"
```

### Running the Client
```bash
./client -d 23456 -s 104857600 -p 12345 -i 192.168.5.7 -r 192.168.5.6 -v -f
```

## Installation

### Prerequisites
Ensure `rdma-core` and required libraries are installed:

```bash
sudo apt-get install rdma-core
```

### Building the Code

Compile the server and client using GCC:
```bash
gcc server.c -o server -lrdmacm -libverbs -lssl -lcrypto
gcc client.c -o client -lrdmacm -libverbs -lssl -lcrypto
```