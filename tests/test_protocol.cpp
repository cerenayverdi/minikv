// Minimal unit tests for the command parser (no external test framework needed).

#include <cstdio>
#include <string>

#include "kv_store.h"
#include "protocol.h"

namespace {

int failures = 0;

void expect(const std::string& command, const std::string& expected, minikv::KVStore& store) {
    std::string actual = minikv::handle_command(command, store);
    if (actual != expected) {
        std::printf("FAIL: '%s'\n  expected: '%s'\n  actual:   '%s'\n", command.c_str(),
                    expected.c_str(), actual.c_str());
        ++failures;
    }
}

}  // namespace

int main() {
    minikv::KVStore store;

    // Basic commands
    expect("PING", "PONG\n", store);
    expect("SET name ali", "OK\n", store);
    expect("GET name", "VALUE ali\n", store);
    expect("SIZE", "SIZE 1\n", store);
    expect("DEL name", "DELETED\n", store);
    expect("GET name", "NOT_FOUND\n", store);
    expect("DEL name", "NOT_FOUND\n", store);

    // Values may contain spaces; commands are case-insensitive; CRLF is accepted
    expect("set city new york", "OK\n", store);
    expect("get city\r", "VALUE new york\n", store);
    expect("  GET   city  ", "VALUE new york\n", store);

    // Overwrite
    expect("SET city toronto", "OK\n", store);
    expect("GET city", "VALUE toronto\n", store);

    // Errors
    expect("", "ERR empty command\n", store);
    expect("FLY away", "ERR unknown command\n", store);
    expect("SET onlykey", "ERR wrong number of arguments\n", store);
    expect("GET", "ERR wrong number of arguments\n", store);
    expect("GET a b", "ERR wrong number of arguments\n", store);
    expect("PING extra", "ERR wrong number of arguments\n", store);

    if (failures == 0) {
        std::printf("all tests passed\n");
        return 0;
    }
    std::printf("%d test(s) failed\n", failures);
    return 1;
}
