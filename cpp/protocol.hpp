// Shared newline-delimited JSON protocol — identical wire format to the
// Python v2 suite, so C++ and Python agents/servers interoperate.
#pragma once

#include "json.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

// Cap a single message at 4 MiB so a peer cannot exhaust memory.
constexpr size_t MAX_MESSAGE_BYTES = 4 * 1024 * 1024;

class ProtocolError : public std::runtime_error {
public:
    explicit ProtocolError(const std::string& msg) : std::runtime_error(msg) {}
};

// Newline-delimited JSON objects over a stream socket.
class LineConnection {
public:
    explicit LineConnection(int fd) : fd_(fd) {}
    ~LineConnection() { close(); }
    LineConnection(const LineConnection&) = delete;
    LineConnection& operator=(const LineConnection&) = delete;
    LineConnection(LineConnection&& other) noexcept;
    LineConnection& operator=(LineConnection&& other) noexcept;

    int fd() const { return fd_; }
    void close();

    void send(const json::Value& msg);  // throws on error / oversize / non-object
    bool recv(json::Value& out);         // false = peer closed cleanly

private:
    int fd_;
    std::string buffer_;
};

// Safe string field access with a fallback.
std::string str_field(const json::Value& v, const std::string& key,
                      const std::string& fallback = "");

// Dot-path lookup, e.g. get_by_path(metrics, "memory.percent").
// Returns nullptr when the path does not exist.
const json::Value* get_by_path(const json::Value& obj, const std::string& path);

// Threshold check shared by the server and the tests: returns one alert
// object per metric at or above its threshold.
std::vector<json::Value> check_alerts(
    const json::Value& metrics,
    const std::map<std::string, double>& thresholds);
