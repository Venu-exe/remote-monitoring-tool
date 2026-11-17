#include "protocol.hpp"

#include <cerrno>
#include <cctype>
#include <cstring>
#include <ctime>
#include <stdexcept>

#include <sys/socket.h>
#include <unistd.h>

LineConnection::LineConnection(LineConnection&& other) noexcept
    : fd_(other.fd_), buffer_(std::move(other.buffer_)) {
    other.fd_ = -1;
}

LineConnection& LineConnection::operator=(LineConnection&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        buffer_ = std::move(other.buffer_);
        other.fd_ = -1;
    }
    return *this;
}

void LineConnection::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void LineConnection::send(const json::Value& msg) {
    if (!msg.is_object())
        throw ProtocolError("messages must be JSON objects");
    std::string payload = json::dump(msg) + "\n";
    if (payload.size() > MAX_MESSAGE_BYTES)
        throw ProtocolError("outgoing message exceeds size limit");
    size_t sent = 0;
    while (sent < payload.size()) {
        ssize_t n = ::send(fd_, payload.data() + sent, payload.size() - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("send failed: ") + std::strerror(errno));
        }
        sent += static_cast<size_t>(n);
    }
}

bool LineConnection::recv(json::Value& out) {
    char chunk[65536];
    while (true) {
        auto nl = buffer_.find('\n');
        if (nl != std::string::npos) {
            std::string line = buffer_.substr(0, nl);
            buffer_.erase(0, nl + 1);
            bool only_ws = true;
            for (char c : line) {
                if (!std::isspace(static_cast<unsigned char>(c))) {
                    only_ws = false;
                    break;
                }
            }
            if (only_ws) continue;
            json::Value v;
            try {
                v = json::parse(line);
            } catch (const json::parse_error& e) {
                throw ProtocolError(std::string("invalid JSON from peer: ") + e.what());
            }
            if (!v.is_object())
                throw ProtocolError("message must be a JSON object");
            out = std::move(v);
            return true;
        }
        ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
        if (n == 0) return false;
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("recv failed: ") + std::strerror(errno));
        }
        buffer_.append(chunk, static_cast<size_t>(n));
        if (buffer_.size() > MAX_MESSAGE_BYTES)
            throw ProtocolError("incoming message exceeds size limit");
    }
}

std::string str_field(const json::Value& v, const std::string& key,
                      const std::string& fallback) {
    const json::Value* p = v.find(key);
    return (p && p->is_string()) ? p->as_string() : fallback;
}

const json::Value* get_by_path(const json::Value& obj, const std::string& path) {
    const json::Value* cur = &obj;
    size_t start = 0;
    while (true) {
        size_t dot = path.find('.', start);
        std::string part = path.substr(
            start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!part.empty()) {
            cur = cur->find(part);
            if (!cur) return nullptr;
        }
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return cur;
}

std::vector<json::Value> check_alerts(
    const json::Value& metrics,
    const std::map<std::string, double>& thresholds) {
    std::vector<json::Value> alerts;
    for (const auto& kv : thresholds) {
        const json::Value* v = get_by_path(metrics, kv.first);
        if (v && v->is_number() && v->number >= kv.second) {
            json::Value a = json::Value::object();
            a["timestamp"] = static_cast<double>(std::time(nullptr));
            a["metric"] = kv.first;
            a["value"] = v->number;
            a["threshold"] = kv.second;
            alerts.push_back(std::move(a));
        }
    }
    return alerts;
}
