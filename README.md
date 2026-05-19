
# Multithreaded HTTP Server in C

## Project Description

This project implements a simple multithreaded HTTP server in C.

The server accepts client connections over TCP, reads HTTP requests, and sends HTTP responses based on the requested resource.

The project uses a thread pool to handle multiple client requests efficiently.

---

## Features

- TCP socket server
- HTTP request parsing
- Support for GET requests
- Multithreaded request handling
- Thread pool implementation
- Static file serving
- Directory handling
- HTTP error responses
- Connection closing after each response

---

## Files

- `server.c`
- `threadpool.c`
- `threadpool.h`
- `README.md`
- `Makefile`

---

## Compilation

```bash
make
````

Or manually:

```bash
gcc -Wall -pthread -o server server.c threadpool.c
```

---

## Running the Server

```bash
./server <port> <pool-size> <max-queue-size> <max-number-of-requests>
```

Example:

```bash
./server 8080 4 10 100
```

Then open in the browser:

```text
http://localhost:8080/
```

---

## Supported Responses

The server handles common HTTP responses such as:

* `200 OK`
* `302 Found`
* `400 Bad Request`
* `403 Forbidden`
* `404 Not Found`
* `501 Not Supported`
* `500 Internal Server Error`

---

## Concepts Used

* C Programming
* TCP Sockets
* HTTP Protocol
* Multithreading
* pthreads
* Thread Pool
* Synchronization
* File System Handling

---

## Author

Sujood Totah
