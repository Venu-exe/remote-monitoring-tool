// C++ collector server / dashboard — multi-agent, token-authenticated,
// threshold alerts. For machines you own or are authorized to monitor.
// Wire protocol is identical to the Python v2 suite (interoperable).

#include "json.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <atomic>
#include <arpa/inet.h>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <random>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {

constexpr const char* SERVER_VERSION = "3.0.0-cpp";

double now_seconds() { return static_cast<double>(std::time(nullptr)); }

std::string random_hex(size_t bytes) {
    static std::mt19937_64 rng {std::random_device {}()};
    static std::mutex rng_mutex;
    static const char* digits = "0123456789abcdef";
    std::lock_guard<std::mutex> g(rng_mutex);
    std::string out;
    out.reserve(bytes * 2);
    for (size_t i = 0; i < bytes; ++i) {
        unsigned int b = static_cast<unsigned int>(rng() & 0xFF);
        out += digits[b >> 4];
        out += digits[b & 0xF];
    }
    return out;
}

bool secure_compare(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string fmt_time(double ts) {
    std::time_t t = static_cast<std::time_t>(ts);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
    return buf;
}

std::string num_str(const json::Value& v, const char* key) {
    const json::Value* p = v.find(key);
    if (!p || !p->is_number()) return "?";
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1) << p->as_number();
    return ss.str();
}

std::string path_num(const json::Value& v, const std::string& path) {
    const json::Value* p = get_by_path(v, path);
    if (!p || !p->is_number()) return "?";
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1) << p->as_number();
    return ss.str();
}

struct PendingRequest {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    json::Value result;
};

struct ClientSession {
    std::string client_id;
    LineConnection conn;
    std::string address;
    json::Value hello;
    double registered_at;
    double last_seen;
    json::Value last_metrics;  // Null until the first heartbeat
    std::vector<std::pair<double, json::Value>> history;
    std::mutex lock;       // guards last_metrics / history / pending
    std::mutex send_lock;  // serializes writes to the socket
    std::map<std::string, std::shared_ptr<PendingRequest>> pending;
    bool alive = true;

    ClientSession(std::string id, LineConnection c, std::string addr, json::Value h)
        : client_id(std::move(id)), conn(std::move(c)), address(std::move(addr)),
          hello(std::move(h)), registered_at(now_seconds()), last_seen(now_seconds()) {}

    void send(const json::Value& msg) {
        std::lock_guard<std::mutex> g(send_lock);
        conn.send(msg);
    }
};

class CollectorServer {
public:
    CollectorServer(std::string host, int port, std::string token,
                    std::map<std::string, double> thresholds, size_t max_history = 200)
        : host_(std::move(host)), port_(port), token_(std::move(token)),
          thresholds_(std::move(thresholds)), max_history_(max_history) {}

    ~CollectorServer() { stop(); }

    void start() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0)
            throw std::runtime_error(std::string("socket(): ") + std::strerror(errno));
        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port_));
        if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1)
            throw std::runtime_error("invalid bind IP: " + host_);
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
            throw std::runtime_error("bind(" + host_ + ":" + std::to_string(port_) + "): " +
                                     std::strerror(errno));
        if (::listen(listen_fd_, 16) != 0)
            throw std::runtime_error(std::string("listen(): ") + std::strerror(errno));
        sockaddr_in bound {};
        socklen_t blen = sizeof(bound);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &blen);
        port_ = ntohs(bound.sin_port);  // resolve ephemeral port 0
        accept_thread_ = std::thread([this] { accept_loop(); });
    }

    void stop() {
        bool expected = false;
        if (!stop_.compare_exchange_strong(expected, true)) return;
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        std::vector<std::shared_ptr<ClientSession>> ss;
        {
            std::lock_guard<std::mutex> g(lock_);
            for (auto& kv : sessions_) ss.push_back(kv.second);
        }
        for (auto& s : ss) {
            s->alive = false;
            s->conn.close();
        }
        if (accept_thread_.joinable()) accept_thread_.join();
    }

    int port() const { return port_; }
    bool stopped() const { return stop_; }

    json::Value send_query(const std::string& client_id, const std::string& command,
                           double timeout_s = 10.0) {
        auto s = find_session(client_id);
        if (!s || !s->alive)
            throw std::runtime_error("unknown or disconnected client: " + client_id);
        auto pr = std::make_shared<PendingRequest>();
        std::string rid = random_hex(8);
        {
            std::lock_guard<std::mutex> g(s->lock);
            s->pending[rid] = pr;
        }
        json::Value cmd = json::Value::object();
        cmd["type"] = "command";
        cmd["request_id"] = rid;
        cmd["command"] = command;
        try {
            s->send(cmd);
        } catch (...) {
            std::lock_guard<std::mutex> g(s->lock);
            s->pending.erase(rid);
            throw std::runtime_error("failed to reach " + client_id);
        }
        std::unique_lock<std::mutex> ul(pr->m);
        if (!pr->cv.wait_for(ul, std::chrono::duration<double>(timeout_s),
                             [&] { return pr->done; })) {
            std::lock_guard<std::mutex> g(s->lock);
            s->pending.erase(rid);
            throw std::runtime_error("no response from " + client_id + " within " +
                                     std::to_string(timeout_s) + "s");
        }
        return pr->result;
    }

    std::map<std::string, json::Value> broadcast(const std::string& command, double timeout_s = 10.0) {
        std::vector<std::string> ids;
        {
            std::lock_guard<std::mutex> g(lock_);
            for (auto& kv : sessions_)
                if (kv.second->alive) ids.push_back(kv.first);
        }
        std::map<std::string, json::Value> results;
        for (auto& id : ids) {
            try {
                results[id] = send_query(id, command, timeout_s);
            } catch (const std::exception& e) {
                json::Value err = json::Value::object();
                err["type"] = "error";
                err["message"] = e.what();
                results[id] = err;
            }
        }
        return results;
    }

    std::vector<json::Value> list_summaries() {
        std::lock_guard<std::mutex> g(lock_);
        std::vector<json::Value> out;
        for (auto& kv : sessions_) {
            auto& s = kv.second;
            std::lock_guard<std::mutex> sg(s->lock);
            json::Value j = json::Value::object();
            j["client_id"] = s->client_id;
            j["address"] = s->address;
            const json::Value* hn = s->last_metrics.find("hostname");
            j["hostname"] =
                (hn && hn->is_string()) ? hn->as_string() : str_field(s->hello, "hostname", "?");
            auto put_num = [&](const char* key, const json::Value* p) {
                j[key] = (p && p->is_number()) ? json::Value(p->as_number()) : json::Value();
            };
            put_num("cpu_percent", s->last_metrics.find("cpu_percent_total"));
            put_num("memory_percent", get_by_path(s->last_metrics, "memory.percent"));
            put_num("disk_percent", get_by_path(s->last_metrics, "disk_root.percent"));
            j["last_seen"] = s->last_seen;
            out.push_back(std::move(j));
        }
        return out;
    }

    std::shared_ptr<ClientSession> find_session(const std::string& id) {
        std::lock_guard<std::mutex> g(lock_);
        auto it = sessions_.find(id);
        return it == sessions_.end() ? nullptr : it->second;
    }

    std::vector<json::Value> get_alerts() {
        std::lock_guard<std::mutex> g(lock_);
        return alerts_;
    }

    void clear_alerts() {
        std::lock_guard<std::mutex> g(lock_);
        alerts_.clear();
    }

private:
    std::string host_;
    int port_;
    std::string token_;
    std::map<std::string, double> thresholds_;
    size_t max_history_;
    std::map<std::string, std::shared_ptr<ClientSession>> sessions_;
    std::vector<json::Value> alerts_;
    std::mutex lock_;        // guards sessions_ / alerts_
    std::mutex print_mutex_;
    std::atomic<bool> stop_ {false};
    int listen_fd_ = -1;
    std::thread accept_thread_;

    void log(const std::string& msg) {
        std::lock_guard<std::mutex> g(print_mutex_);
        std::cout << "[server] " << msg << "\n" << std::flush;
    }

    void accept_loop() {
        while (!stop_) {
            sockaddr_in caddr {};
            socklen_t clen = sizeof(caddr);
            int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&caddr), &clen);
            if (fd < 0) {
                if (stop_) break;
                if (errno == EINTR) continue;
                break;
            }
            std::thread([this, fd, caddr] { handle_client(fd, caddr); }).detach();
        }
    }

    void handle_client(int fd, sockaddr_in caddr) {
        char ip[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &caddr.sin_addr, ip, sizeof(ip));
        std::string addr = std::string(ip) + ":" + std::to_string(ntohs(caddr.sin_port));
        std::shared_ptr<ClientSession> session;
        try {
            LineConnection conn(fd);
            json::Value hello;
            bool got = conn.recv(hello);
            const json::Value* htype = got ? hello.find("type") : nullptr;
            const json::Value* htok = got ? hello.find("token") : nullptr;
            bool ok = htype && htype->is_string() && htype->as_string() == "hello" && htok &&
                      htok->is_string() && secure_compare(htok->as_string(), token_);
            if (!ok) {
                try {
                    json::Value denied = json::Value::object();
                    denied["type"] = "denied";
                    denied["reason"] = "invalid or missing token";
                    conn.send(denied);
                } catch (...) {}
                log("rejected unauthenticated peer " + addr);
                conn.close();  // always close rejected peers — no FD leak
                return;
            }
            std::string hostname = str_field(hello, "hostname", ip);
            std::string client_id = hostname + "-" + random_hex(3);
            session = std::make_shared<ClientSession>(client_id, std::move(conn), addr, hello);
            {
                std::lock_guard<std::mutex> g(lock_);
                sessions_[client_id] = session;
            }
            json::Value welcome = json::Value::object();
            welcome["type"] = "welcome";
            welcome["client_id"] = client_id;
            welcome["server_version"] = SERVER_VERSION;
            session->send(welcome);
            log("client connected: " + client_id + " from " + addr);
            message_loop(session);
        } catch (const std::exception& e) {
            log("connection error from " + addr + ": " + e.what());
        }
        if (session) {
            session->alive = false;
            {
                std::lock_guard<std::mutex> g(lock_);
                sessions_.erase(session->client_id);
            }
            {
                std::lock_guard<std::mutex> g(session->lock);
                for (auto& kv : session->pending) {
                    {
                        std::lock_guard<std::mutex> pg(kv.second->m);
                        kv.second->done = true;
                    }
                    kv.second->cv.notify_all();
                }
            }
            log("client disconnected: " + session->client_id);
        }
        // The socket is closed by LineConnection's destructor — including for
        // rejected peers above — so no file descriptor is ever leaked.
    }

    void message_loop(std::shared_ptr<ClientSession> s) {
        while (!stop_) {
            json::Value msg;
            if (!s->conn.recv(msg)) return;
            s->last_seen = now_seconds();
            std::string type = str_field(msg, "type");
            if (type == "metrics") {
                store_metrics(s, msg);
            } else if (type == "response" || type == "error") {
                route_response(s, msg);
            }
        }
    }

    void store_metrics(std::shared_ptr<ClientSession> s, const json::Value& msg) {
        {
            std::lock_guard<std::mutex> g(s->lock);
            s->last_metrics = msg;
            const json::Value* ts = msg.find("timestamp");
            double t = (ts && ts->is_number()) ? ts->as_number() : now_seconds();
            s->history.emplace_back(t, msg);
            if (s->history.size() > max_history_)
                s->history.erase(s->history.begin(),
                                 s->history.begin() + (s->history.size() - max_history_));
        }
        for (auto alert : check_alerts(msg, thresholds_)) {
            alert["client_id"] = s->client_id;
            {
                std::lock_guard<std::mutex> g(lock_);
                alerts_.push_back(alert);
            }
            std::ostringstream ss;
            ss << "[alert] " << s->client_id << ": " << str_field(alert, "metric") << " = "
               << std::fixed << std::setprecision(1) << alert.at("value").as_number()
               << "% (threshold " << alert.at("threshold").as_number() << "%)";
            log(ss.str());
        }
    }

    void route_response(std::shared_ptr<ClientSession> s, const json::Value& msg) {
        std::string rid = str_field(msg, "request_id");
        std::shared_ptr<PendingRequest> pr;
        {
            std::lock_guard<std::mutex> g(s->lock);
            auto it = s->pending.find(rid);
            if (it != s->pending.end()) {
                pr = it->second;
                s->pending.erase(it);
            }
        }
        if (pr) {
            {
                std::lock_guard<std::mutex> g(pr->m);
                pr->result = msg;
                pr->done = true;
            }
            pr->cv.notify_all();
        }
    }
};

constexpr const char* HELP =
    "Commands:\n"
    "  list                          show connected agents\n"
    "  query <client_id> <command>   send a read-only command to one agent\n"
    "                                (try: ping | get_system_info | get memory_info.virtual_memory.percent)\n"
    "  broadcast <command>           send a command to every connected agent\n"
    "  metrics <client_id>           show the agent's latest heartbeat\n"
    "  history <client_id> [n]       show the last n heartbeat samples (default 10)\n"
    "  alerts [clear]                show triggered threshold alerts (or clear them)\n"
    "  help                          show this help\n"
    "  exit                          shut the server down\n";

void console_loop(CollectorServer& server) {
    std::cout << HELP;
    std::string line;
    while (true) {
        std::cout << "monitor> " << std::flush;
        if (!std::getline(std::cin, line)) break;
        std::istringstream iss(line);
        std::string cmd;
        if (!(iss >> cmd)) continue;
        cmd = lower(cmd);
        try {
            if (cmd == "exit" || cmd == "quit") break;
            else if (cmd == "help") {
                std::cout << HELP;
            } else if (cmd == "list") {
                auto summaries = server.list_summaries();
                if (summaries.empty()) {
                    std::cout << "no agents connected\n";
                } else {
                    for (auto& s : summaries) {
                        std::cout << "  " << std::left << std::setw(24)
                                  << str_field(s, "client_id") << " " << std::setw(22)
                                  << str_field(s, "address") << " cpu=" << num_str(s, "cpu_percent")
                                  << " mem=" << num_str(s, "memory_percent") << "%"
                                  << " disk=" << num_str(s, "disk_percent") << "%"
                                  << " last_seen=" << fmt_time(s.at("last_seen").as_number())
                                  << "\n";
                    }
                }
            } else if (cmd == "query") {
                std::string id, rest;
                iss >> id;
                std::getline(iss, rest);
                rest = trim(rest);
                if (id.empty() || rest.empty()) {
                    std::cout << "usage: query <client_id> <command>\n";
                    continue;
                }
                json::Value reply = server.send_query(id, rest);
                if (str_field(reply, "type") == "response")
                    std::cout << json::dump_pretty(reply.at("payload")) << "\n";
                else
                    std::cout << str_field(reply, "message", "unexpected reply") << "\n";
            } else if (cmd == "broadcast") {
                std::string rest;
                std::getline(iss, rest);
                rest = trim(rest);
                if (rest.empty()) {
                    std::cout << "usage: broadcast <command>\n";
                    continue;
                }
                for (auto& kv : server.broadcast(rest)) {
                    std::cout << "--- " << kv.first << " ---\n";
                    const json::Value& r = kv.second;
                    if (str_field(r, "type") == "response" && r.find("payload"))
                        std::cout << json::dump_pretty(r.at("payload")) << "\n";
                    else
                        std::cout << str_field(r, "message", json::dump(r)) << "\n";
                }
            } else if (cmd == "metrics") {
                std::string id;
                iss >> id;
                auto s = server.find_session(id);
                if (!s) {
                    std::cout << "unknown client: " << id << "\n";
                    continue;
                }
                std::lock_guard<std::mutex> g(s->lock);
                if (s->last_metrics.is_null())
                    std::cout << "no metrics yet\n";
                else
                    std::cout << json::dump_pretty(s->last_metrics) << "\n";
            } else if (cmd == "history") {
                std::string id;
                iss >> id;
                size_t n = 10;
                if (!(iss >> n)) n = 10;
                auto s = server.find_session(id);
                if (!s) {
                    std::cout << "unknown client: " << id << "\n";
                    continue;
                }
                std::lock_guard<std::mutex> g(s->lock);
                size_t start = n >= s->history.size() ? 0 : s->history.size() - n;
                for (size_t i = start; i < s->history.size(); ++i) {
                    auto& m = s->history[i].second;
                    std::cout << "  " << fmt_time(s->history[i].first)
                              << " cpu=" << path_num(m, "cpu_percent_total") << "%"
                              << " mem=" << path_num(m, "memory.percent") << "%"
                              << " disk=" << path_num(m, "disk_root.percent") << "%\n";
                }
            } else if (cmd == "alerts") {
                std::string sub;
                iss >> sub;
                if (lower(sub) == "clear") {
                    server.clear_alerts();
                    std::cout << "alerts cleared\n";
                } else {
                    auto alerts = server.get_alerts();
                    if (alerts.empty()) {
                        std::cout << "no alerts triggered\n";
                    } else {
                        for (auto& a : alerts) {
                            std::cout << "  " << fmt_time(a.at("timestamp").as_number()) << " "
                                      << str_field(a, "client_id") << ": "
                                      << str_field(a, "metric") << " = "
                                      << a.at("value").as_number() << "% (threshold "
                                      << a.at("threshold").as_number() << "%)\n";
                        }
                    }
                }
            } else {
                std::cout << "unknown command — type 'help'\n";
            }
        } catch (const std::exception& e) {
            std::cout << "error: " << e.what() << "\n";
        }
    }
    server.stop();
}

struct ServerArgs {
    std::string host = "127.0.0.1";
    int port = 5000;
    std::string token;
    double cpu_th = 90.0, mem_th = 90.0, disk_th = 90.0;
};

void usage() {
    std::cout << "Usage: server --host H --port P --token T [--cpu-threshold N] "
                 "[--mem-threshold N] [--disk-threshold N]\n"
                 "  --token may also come from the MONITOR_TOKEN environment variable.\n";
}

ServerArgs parse_args(int argc, char** argv) {
    ServerArgs a;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc)
                throw std::runtime_error(std::string("missing value for ") + name);
            return argv[++i];
        };
        if (arg == "--host") a.host = next("--host");
        else if (arg == "--port") a.port = std::stoi(next("--port"));
        else if (arg == "--token") a.token = next("--token");
        else if (arg == "--cpu-threshold") a.cpu_th = std::stod(next("--cpu-threshold"));
        else if (arg == "--mem-threshold") a.mem_th = std::stod(next("--mem-threshold"));
        else if (arg == "--disk-threshold") a.disk_th = std::stod(next("--disk-threshold"));
        else if (arg == "--help" || arg == "-h") { usage(); std::exit(0); }
        else throw std::runtime_error("unknown argument: " + arg);
    }
    if (a.token.empty()) {
        if (const char* env = std::getenv("MONITOR_TOKEN")) a.token = env;
    }
    return a;
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
    ServerArgs a;
    try {
        a = parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        usage();
        return 2;
    }
    if (a.token.empty()) {
        a.token = random_hex(16);
        std::cerr << "warning: no --token / MONITOR_TOKEN set; generated an ephemeral token:\n"
                  << "  " << a.token << "\nstart agents with: MONITOR_TOKEN=" << a.token
                  << " ./build/agent ...\n";
    }
    std::map<std::string, double> thresholds;
    thresholds["cpu_percent_total"] = a.cpu_th;
    thresholds["memory.percent"] = a.mem_th;
    thresholds["disk_root.percent"] = a.disk_th;
    CollectorServer server(a.host, a.port, a.token, thresholds);
    try {
        server.start();
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    std::cout << "Collector server v" << SERVER_VERSION << " listening on " << a.host << ":"
              << server.port() << "\n";
    console_loop(server);
    std::cout << "server shut down\n";
    return 0;
}
