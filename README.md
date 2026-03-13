# Multi-Threaded HTTP Proxy Server with Caching

A high-performance HTTP/1.1 Proxy Server written in C. This server acts as an intermediary between a client (e.g., a web browser) and a remote web server. It features a robust caching mechanism and utilizes multi-threading to handle multiple concurrent client connections efficiently.

## 🚀 Features

* **Multi-Threading:** Uses `pthread` to spawn worker threads for every incoming connection.
* **Concurrency Control:** Implements semaphores to limit the number of simultaneous active connections (default limit: 400).
* **LRU Caching:** A custom-built Linked List cache with a **Least Recently Used (LRU)** eviction policy to optimize performance and reduce bandwidth.
* **Request Parsing:** Custom HTTP parser that deconstructs raw HTTP strings into manageable C structures (Method, Host, Path, Port, etc.).
* **Error Handling:** Custom HTML error pages for `400 Bad Request`, `403 Forbidden`, `404 Not Found`, and `500 Internal Server Error`.

## 🛠 Architecture

The proxy operates on a **Man-in-the-Middle** flow:
1. Client sends an HTTP request to the Proxy.
2. Proxy checks if the requested URL is in the **Cache**.
3. If **found**, it serves the data directly from memory.
4. If **not found**, the Proxy opens a socket to the remote server, fetches the data, sends it to the client, and stores a copy in the Cache.



## 📋 Prerequisites

* GCC Compiler
* Linux/Unix-based environment
* `pthread` and `semaphore` libraries (usually standard on Linux)

## 🔧 Compilation

Use the provided `Makefile` or compile manually using the following command:

gcc -pthread proxy_parse.c ps_with_cache.c -o proxy


