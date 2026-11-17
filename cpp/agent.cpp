// C++ monitoring agent — read-only telemetry for machines you own or are
// explicitly authorized to monitor. Same wire protocol as the Python suite.

#include "json.hpp"
#include "metrics.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

constexpr const char* AGENT_VERSION = "3.0.0-cpp";

struct Args {
    std::string host = "127.0.0.1";
    int port = 5000;
    std::string token;
    double interval = 5.0;
    int max_retries = -1;  // -1 = retry forever
};

void usage() {
    std::cout << "Usage: agent --host H --port P --token T [--interval SECONDS] [--max-retries N]\n"
                 "  --token may also come from the MONITOR_TOKEN environment variable.\n";
}

Args parse_args(int argc, char** argv) {
    Args a;
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
        else if (arg == "--interval") a.interval = std::stod(next("--interval"));
        else if (arg == "--max-retries") a.max_retries = std::stoi(next("--max-retries"));
        else if (arg == "--help" || arg == "-h") { usage(); std::exit(0); }
        else throw std::runtime_error("unknown argument: " + arg);
    }
    if (a.token.empty()) {
        if (const char* env = std::getenv("MONITOR_TOKEN")) a.token = env;
    }
    return a;
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

class Agent {
public:
    explicit Agent(Args args) : args_(std::move(args)) {}

    void run() {
        std::mt19937 rng{std::random_device{}()};
        int attempt = 0;
        while (true) {
            try {
                LineConnection conn = connect();
                attempt = 0;
                std::cout << "[agent] connected to " << args_.host << ":" << args_.port
                          << " as '" << client_id_ << "'\n" << std::flush;
                session(conn);
            } catch (const AuthError& e) {
                std::cout << "[agent] authentication failed: " << e.what()
                          << " (check the shared token)\n" << std::flush;
                return;
            } catch (const CloseRequested&) {
                std::cout << "[agent] server asked us to disconnect\n" << std::flush;
                return;
            } catch (const std::exception& e) {
                ++attempt;
                if (args_.max_retries >= 0 && attempt > args_.max_retries) {
                    std::cout << "[agent] giving up after " << attempt << " attempts: "
                              << e.what() << "\n" << std::flush;
                    return;
                }
                double wait = std::min(30.0, std::pow(2.0, attempt - 1)) +
                              std::uniform_real_distribution<double>(0.0, 0.5)(rng);
                std::cout << "[agent] connection problem (" << e.what() << "); reconnecting in "
                          << wait << "s\n" << std::flush;
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(static_cast<long long>(wait * 1000)));
            }
        }
    }

private:
    struct AuthError : std::runtime_error {
        explicit AuthError(const std::string& m) : std::runtime_error(m) {}
    };
    struct CloseRequested : std::runtime_error {
        CloseRequested() : std::runtime_error("close requested") {}
    };

    Args args_;
    std::string client_id_;

    LineConnection connect() {
        addrinfo hints {};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        std::string port_str = std::to_string(args_.port);
        if (::getaddrinfo(args_.host.c_str(), port_str.c_str(), &hints, &res) != 0)
            throw std::runtime_error("cannot resolve host: " + args_.host);
        int fd = -1;
        for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
            fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (fd < 0) continue;
            if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
            ::close(fd);
            fd = -1;
        }
        ::freeaddrinfo(res);
        if (fd < 0) throw std::runtime_error("connection failed");

        LineConnection conn(fd);
        json::Value hello = json::Value::object();
        hello["type"] = "hello";
        hello["token"] = args_.token;
        hello["hostname"] = system_hostname();
        hello["agent_version"] = AGENT_VERSION;
        conn.send(hello);

        json::Value reply;
        if (!conn.recv(reply)) throw std::runtime_error("server closed during handshake");
        std::string type = str_field(reply, "type");
        if (type == "denied") throw AuthError(str_field(reply, "reason", "access denied"));
        if (type != "welcome") throw ProtocolError("unexpected handshake reply: " + json::dump(reply));
        client_id_ = str_field(reply, "client_id");
        return conn;
    }

    void session(LineConnection& conn) {
        using clock = std::chrono::steady_clock;
        auto next_report = clock::now();
        while (true) {
            auto now = clock::now();
            if (now >= next_report) {
                conn.send(collect_metrics());
                next_report =
                    now + std::chrono::milliseconds(static_cast<long long>(args_.interval * 1000));
            }
            pollfd pfd {conn.fd(), POLLIN, 0};
            int r = ::poll(&pfd, 1, 100);
            if (r < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error(std::string("poll failed: ") + std::strerror(errno));
            }
            if (r == 0) continue;
            if (pfd.revents & POLLIN) {
                json::Value msg;
                if (!conn.recv(msg)) throw std::runtime_error("server closed the connection");
                std::string type = str_field(msg, "type");
                if (type == "command") {
                    handle_command(conn, msg);
                } else if (type == "ping") {
                    json::Value pong = json::Value::object();
                    pong["type"] = "pong";
                    pong["timestamp"] = static_cast<double>(std::time(nullptr));
                    conn.send(pong);
                }
            } else if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
                throw std::runtime_error("server closed the connection");
            }
        }
    }

    void handle_command(LineConnection& conn, const json::Value& msg) {
        std::string command = str_field(msg, "command");
        std::string rid = str_field(msg, "request_id");
        std::string cmd = lower(trim(command));
        if (cmd == "exit" || cmd == "bye") throw CloseRequested();

        json::Value reply = json::Value::object();
        reply["request_id"] = rid;
        if (cmd == "ping") {
            reply["type"] = "response";
            json::Value p = json::Value::object();
            p["pong"] = static_cast<double>(std::time(nullptr));
            reply["payload"] = p;
        } else if (cmd == "get_system_info") {
            reply["type"] = "response";
            reply["payload"] = full_system_info();
        } else if (cmd.rfind("get ", 0) == 0) {
            std::string path = trim(command.substr(4));
            const json::Value* v = get_by_path(full_system_info(), path);
            if (v == nullptr) {
                reply["type"] = "error";
                reply["message"] = "path not found: " + path;
            } else {
                reply["type"] = "response";
                json::Value p = json::Value::object();
                p["path"] = path;
                p["value"] = *v;
                reply["payload"] = p;
            }
        } else {
            reply["type"] = "error";
            reply["message"] = "unknown command: " + command;
        }
        conn.send(reply);
    }
};

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
    Args args;
    try {
        args = parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        usage();
        return 2;
    }
    if (args.token.empty()) {
        std::cerr << "error: provide --token or set the MONITOR_TOKEN environment variable\n";
        return 2;
    }
    Agent agent(std::move(args));
    agent.run();
    return 0;
}
