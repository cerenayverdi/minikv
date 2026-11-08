#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>

namespace minikv {

// In-memory key-value store.
// The server runs a single-threaded event loop, so no locking is needed here.
class KVStore {
public:
    void set(const std::string& key, const std::string& value) { data_[key] = value; }

    std::optional<std::string> get(const std::string& key) const {
        auto it = data_.find(key);
        if (it == data_.end()) return std::nullopt;
        return it->second;
    }

    bool del(const std::string& key) { return data_.erase(key) > 0; }

    std::size_t size() const { return data_.size(); }

private:
    std::unordered_map<std::string, std::string> data_;
};

}  // namespace minikv
