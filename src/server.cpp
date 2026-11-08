// minikv-server: a single-threaded, epoll-based TCP key-value server.
//
// Usage: minikv-server [port]   (default port: 6380)

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

#include "kv_store.h"
#include "protocol.h"

namespace {

constexpr int kDefaultPort = 6380;
constexpr int kMaxEvents = 64;
constexpr std::size_t kReadChunk = 4096;
constexpr std::size_t kMaxLineLength = 64 * 1024;  // protects against clients that never send '\n'

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

struct Connection {
    std::string in;            // bytes received but not yet parsed into full lines
    std::string out;           // responses waiting to be sent
    bool want_write = false;   // is EPOLLOUT currently registered?
};

void die(const char* what) {
    std::perror(what);
    std::exit(EXIT_FAILURE);
}

bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags != -1 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

int create_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) die("socket");

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) die("bind");
    if (listen(fd, SOMAXCONN) < 0) die("listen");
    if (!set_nonblocking(fd)) die("fcntl");
    return fd;
}

class Server {
public:
    explicit Server(int port) : listen_fd_(create_listen_socket(port)) {
        epoll_fd_ = epoll_create1(0);
        if (epoll_fd_ < 0) die("epoll_create1");

        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = listen_fd_;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &ev) < 0) die("epoll_ctl listen");
    }

    ~Server() {
        for (auto& [fd, conn] : conns_) close(fd);
        close(listen_fd_);
        close(epoll_fd_);
    }

    void run() {
        epoll_event events[kMaxEvents];
        while (!g_stop) {
            int n = epoll_wait(epoll_fd_, events, kMaxEvents, -1);
            if (n < 0) {
                if (errno == EINTR) continue;  // interrupted by a signal; loop re-checks g_stop
                die("epoll_wait");
            }
            for (int i = 0; i < n; ++i) {
                int fd = events[i].data.fd;
                uint32_t ev = events[i].events;
                if (fd == listen_fd_) {
                    accept_clients();
                } else {
                    handle_client(fd, ev);
                }
            }
        }
        std::printf("\nshutting down, %zu keys in store\n", store_.size());
    }

private:
    void accept_clients() {
        // The listen socket is non-blocking, so accept until the backlog is empty.
        while (true) {
            int fd = accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK);
            if (fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                if (errno == EINTR) continue;
                std::perror("accept4");
                return;
            }

            // Responses are small; send them immediately instead of waiting (Nagle).
            int yes = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

            epoll_event ev{};
            ev.events = EPOLLIN;
            ev.data.fd = fd;
            if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
                std::perror("epoll_ctl add client");
                close(fd);
                continue;
            }
            conns_[fd] = Connection{};
        }
    }

    void handle_client(int fd, uint32_t events) {
        auto it = conns_.find(fd);
        if (it == conns_.end()) return;
        Connection& conn = it->second;

        if (events & (EPOLLERR | EPOLLHUP)) {
            close_client(fd);
            return;
        }
        if ((events & EPOLLIN) && !read_and_process(fd, conn)) {
            close_client(fd);
            return;
        }
        if (!flush(fd, conn)) {
            close_client(fd);
        }
    }

    // Reads everything currently available, then executes every complete line.
    // Returns false if the connection should be closed.
    bool read_and_process(int fd, Connection& conn) {
        char buf[kReadChunk];
        while (true) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n > 0) {
                conn.in.append(buf, static_cast<std::size_t>(n));
                continue;
            }
            if (n == 0) return false;  // client closed the connection
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            return false;
        }

        // TCP is a byte stream: one read() may hold half a command or several commands.
        std::size_t start = 0;
        while (true) {
            std::size_t newline = conn.in.find('\n', start);
            if (newline == std::string::npos) break;
            std::string_view line(conn.in.data() + start, newline - start);
            conn.out += minikv::handle_command(line, store_);
            start = newline + 1;
        }
        conn.in.erase(0, start);

        return conn.in.size() <= kMaxLineLength;
    }

    // Writes as much pending output as the socket accepts. If the kernel buffer
    // is full, registers EPOLLOUT so we are woken up when we can write again.
    bool flush(int fd, Connection& conn) {
        std::size_t sent = 0;
        while (sent < conn.out.size()) {
            ssize_t n = send(fd, conn.out.data() + sent, conn.out.size() - sent, MSG_NOSIGNAL);
            if (n > 0) {
                sent += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            return false;
        }
        conn.out.erase(0, sent);

        bool need_write = !conn.out.empty();
        if (need_write != conn.want_write) {
            epoll_event ev{};
            ev.events = EPOLLIN | (need_write ? EPOLLOUT : 0u);
            ev.data.fd = fd;
            if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0) return false;
            conn.want_write = need_write;
        }
        return true;
    }

    void close_client(int fd) {
        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        conns_.erase(fd);
    }

    int listen_fd_;
    int epoll_fd_;
    minikv::KVStore store_;
    std::unordered_map<int, Connection> conns_;
};

}  // namespace

int main(int argc, char** argv) {
    int port = argc > 1 ? std::atoi(argv[1]) : kDefaultPort;
    if (port <= 0 || port > 65535) {
        std::fprintf(stderr, "invalid port\n");
        return EXIT_FAILURE;
    }

    // No SA_RESTART: we want epoll_wait to return EINTR so the loop can exit cleanly.
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    Server server(port);
    std::printf("minikv listening on port %d\n", port);
    std::fflush(stdout);
    server.run();
    return EXIT_SUCCESS;
}
