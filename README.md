# minikv

A small in-memory key-value server written in C++20 for Linux. Clients connect over TCP and send simple text commands. The server handles many concurrent connections in a single thread using `epoll` and non-blocking sockets.

## Build

Requires Linux, a C++20 compiler and CMake.

```bash
cmake -B build
cmake --build build
ctest --test-dir build        # run unit tests
```

## Usage

Start the server (default port 6380):

```bash
./build/minikv-server 6380
```

Talk to it from another terminal with `nc`:

```
$ nc localhost 6380
SET city new york
OK
GET city
VALUE new york
DEL city
DELETED
GET city
NOT_FOUND
```

Stop the server with `Ctrl+C`; it shuts down cleanly.

## Protocol

One command per line, terminated by `\n` (`\r\n` also accepted). Commands are case-insensitive.

| Command             | Response                         |
|---------------------|----------------------------------|
| `PING`              | `PONG`                           |
| `SET <key> <value>` | `OK` (value may contain spaces)  |
| `GET <key>`         | `VALUE <value>` or `NOT_FOUND`   |
| `DEL <key>`         | `DELETED` or `NOT_FOUND`         |
| `SIZE`              | `SIZE <n>`                       |
| anything invalid    | `ERR <message>`                  |

## Design

**Single-threaded event loop.** One thread waits on `epoll` for any socket that is ready, then reads, executes commands and writes responses. Because only one thread touches the store, no locks are needed. This is the same model Redis uses; it avoids the memory and context-switch cost of one thread per connection.

**Message framing.** TCP is a byte stream, so one `read()` may return half a command or several commands at once. Each connection keeps an input buffer; only complete lines (ending in `\n`) are executed, and the leftover bytes wait for the next read. Lines longer than 64 KB close the connection.

**Partial writes.** If the kernel send buffer is full, unsent data stays in a per-connection output buffer and the socket is registered for `EPOLLOUT`, so the server is woken up when it can write again. `EPOLLOUT` is removed once the buffer is empty to avoid busy wake-ups.

**Low latency.** `TCP_NODELAY` disables Nagle's algorithm, so small responses are sent immediately instead of being delayed to batch with later data.

**Robustness.** `SIGPIPE` is ignored (writes to a closed socket return an error instead of killing the process), and `SIGINT`/`SIGTERM` interrupt `epoll_wait` so the loop exits and closes all sockets.

## Benchmark

`minikv-bench` opens N connections, each in its own thread, and alternates `SET` and `GET` requests, waiting for every response before sending the next one. It reports throughput and latency percentiles.

```bash
./build/minikv-bench [clients] [requests_per_client]
./build/minikv-bench 8 10000
```

Results on an Intel Core i7-1165G7 @ 2.80 GHz, Ubuntu on Windows (WSL2). Client and server run on the same machine; every run completed with zero errors.

| Clients | Requests | Throughput (req/s) | p50 (µs) | p99 (µs) |
|---------|----------|--------------------|----------|----------|
| 1       | 10,000   | 11,869             | 78       | 158      |
| 8       | 80,000   | 26,501             | 249      | 631      |
| 64      | 640,000  | 23,785             | 2,588    | 5,086    |

**What the numbers show.** Going from 1 to 8 clients more than doubles throughput: with one client the server spends most of its time waiting for the next request, and more clients fill those gaps. Around 8 clients the single event-loop thread is saturated, so adding more clients no longer raises throughput; requests simply wait in line and latency grows.

This matches Little's law (average latency ≈ requests in flight ÷ throughput): at 64 clients, 64 ÷ 23,785 req/s ≈ 2.69 ms, close to the measured average of 2.66 ms. Scaling further would need more threads, for example one event loop per core.

## Inspecting traffic with Wireshark

Capture loopback traffic on the server port, for example:

```bash
sudo tcpdump -i lo port 6380 -w minikv.pcap
```

Open the file in Wireshark (or capture live on the loopback interface) with the display filter `tcp.port == 6380`, then use *Follow → TCP Stream* to see the commands and responses as plain text. You can also observe the TCP handshake, how several commands can share one segment, and the effect of `TCP_NODELAY`.

## Project layout

```
include/kv_store.h      in-memory store
include/protocol.h      command parser interface
src/protocol.cpp        command parsing and execution
src/server.cpp          epoll server
src/bench.cpp           load generator
tests/test_protocol.cpp unit tests
```

## Possible extensions

- Pipelining benchmark mode (send many requests before reading responses)
- Key expiry (`SET key value EX seconds`)
- Persistence to disk with an append-only log
- Multi-threaded version with one event loop per core, compared against the single-threaded one
