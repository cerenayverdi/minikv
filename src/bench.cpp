// minikv-bench: load generator that measures throughput and latency.
//
// Usage: minikv-bench [clients] [requests_per_client] [host] [port]
// Defaults: 8 clients, 10000 requests each, 127.0.0.1, 6380
//
// Each client opens its own TCP connection and alternates SET and GET requests,
// waiting for each response before sending the next one (closed-loop load).

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Config {
    int clients = 8;
    int requests = 10000;
    std::string host = "127.0.0.1";
    int port = 6380;
};

int connect_to(const Config& cfg) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(cfg.port));
    if (inet_pton(AF_INET, cfg.host.c_str(), &addr.sin_addr) != 1 ||
        connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    return fd;
}

bool send_all(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

// Reads one '\n'-terminated line. `buffer` keeps leftover bytes between calls.
bool read_line(int fd, std::string& buffer, std::string& line) {
    while (true) {
        std::size_t newline = buffer.find('\n');
        if (newline != std::string::npos) {
            line.assign(buffer, 0, newline);
            buffer.erase(0, newline + 1);
            return true;
        }
        char chunk[4096];
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) return false;
        buffer.append(chunk, static_cast<std::size_t>(n));
    }
}

void run_client(const Config& cfg, int id, std::vector<double>& latencies_us,
                std::atomic<int>& errors) {
    int fd = connect_to(cfg);
    if (fd < 0) {
        errors += cfg.requests;
        return;
    }

    latencies_us.reserve(static_cast<std::size_t>(cfg.requests));
    std::string buffer, response;

    for (int i = 0; i < cfg.requests; ++i) {
        std::string key = "client" + std::to_string(id) + ":key" + std::to_string(i / 2);
        bool is_set = (i % 2 == 0);
        std::string request = is_set ? "SET " + key + " value" + std::to_string(i) + "\n"
                                     : "GET " + key + "\n";

        auto start = Clock::now();
        if (!send_all(fd, request) || !read_line(fd, buffer, response)) {
            errors += cfg.requests - i;
            break;
        }
        auto end = Clock::now();
        latencies_us.push_back(std::chrono::duration<double, std::micro>(end - start).count());

        bool ok = is_set ? response == "OK" : response.rfind("VALUE ", 0) == 0;
        if (!ok) ++errors;
    }
    close(fd);
}

double percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    std::size_t index = static_cast<std::size_t>(p / 100.0 * static_cast<double>(sorted.size() - 1));
    return sorted[index];
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    if (argc > 1) cfg.clients = std::atoi(argv[1]);
    if (argc > 2) cfg.requests = std::atoi(argv[2]);
    if (argc > 3) cfg.host = argv[3];
    if (argc > 4) cfg.port = std::atoi(argv[4]);
    if (cfg.clients <= 0 || cfg.requests <= 0) {
        std::fprintf(stderr, "clients and requests must be positive\n");
        return EXIT_FAILURE;
    }

    std::vector<std::vector<double>> per_client(static_cast<std::size_t>(cfg.clients));
    std::vector<std::thread> threads;
    std::atomic<int> errors{0};

    auto start = Clock::now();
    for (int i = 0; i < cfg.clients; ++i) {
        threads.emplace_back(run_client, std::cref(cfg), i,
                             std::ref(per_client[static_cast<std::size_t>(i)]), std::ref(errors));
    }
    for (auto& t : threads) t.join();
    double seconds = std::chrono::duration<double>(Clock::now() - start).count();

    std::vector<double> all;
    for (auto& v : per_client) all.insert(all.end(), v.begin(), v.end());
    std::sort(all.begin(), all.end());

    double avg = 0.0;
    for (double x : all) avg += x;
    if (!all.empty()) avg /= static_cast<double>(all.size());

    std::printf("clients:      %d\n", cfg.clients);
    std::printf("requests:     %zu (errors: %d)\n", all.size(), errors.load());
    std::printf("duration:     %.2f s\n", seconds);
    std::printf("throughput:   %.0f req/s\n", static_cast<double>(all.size()) / seconds);
    std::printf("latency avg:  %.1f us\n", avg);
    std::printf("latency p50:  %.1f us\n", percentile(all, 50));
    std::printf("latency p99:  %.1f us\n", percentile(all, 99));
    std::printf("latency max:  %.1f us\n", all.empty() ? 0.0 : all.back());

    return errors.load() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
