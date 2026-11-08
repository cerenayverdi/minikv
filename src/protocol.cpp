#include "protocol.h"

#include <cctype>

namespace minikv {
namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    return s;
}

// Removes and returns the next space-separated token from `s`.
std::string_view next_token(std::string_view& s) {
    s = trim(s);
    std::size_t end = s.find(' ');
    std::string_view token = s.substr(0, end);
    s.remove_prefix(end == std::string_view::npos ? s.size() : end);
    return token;
}

const char* const kWrongArgs = "ERR wrong number of arguments\n";

}  // namespace

std::string handle_command(std::string_view line, KVStore& store) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);  // accept CRLF too

    std::string_view rest = line;
    std::string cmd(next_token(rest));
    for (char& c : cmd) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    if (cmd.empty()) return "ERR empty command\n";

    if (cmd == "PING") {
        return trim(rest).empty() ? "PONG\n" : kWrongArgs;
    }

    if (cmd == "SET") {
        std::string_view key = next_token(rest);
        std::string_view value = trim(rest);  // value is the rest of the line, spaces allowed
        if (key.empty() || value.empty()) return kWrongArgs;
        store.set(std::string(key), std::string(value));
        return "OK\n";
    }

    if (cmd == "GET" || cmd == "DEL") {
        std::string_view key = next_token(rest);
        if (key.empty() || !trim(rest).empty()) return kWrongArgs;

        if (cmd == "GET") {
            auto value = store.get(std::string(key));
            return value ? "VALUE " + *value + "\n" : "NOT_FOUND\n";
        }
        return store.del(std::string(key)) ? "DELETED\n" : "NOT_FOUND\n";
    }

    if (cmd == "SIZE") {
        return trim(rest).empty() ? "SIZE " + std::to_string(store.size()) + "\n" : kWrongArgs;
    }

    return "ERR unknown command\n";
}

}  // namespace minikv
