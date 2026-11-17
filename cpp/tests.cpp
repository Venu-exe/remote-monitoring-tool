// Unit tests for the C++ suite: JSON roundtrip, dot-path lookup, threshold
// alerts, metrics smoke test, and the LineConnection protocol over socketpair
// (including the hostile cases: non-object JSON, oversized messages, EOF).
// Build & run with: make test

#include "json.hpp"
#include "metrics.hpp"
#include "protocol.hpp"

#include <cmath>
#include <csignal>
#include <iostream>
#include <map>
#include <string>
#include <thread>

#include <sys/socket.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (cond) {                                                     \
            std::cout << "PASS line " << __LINE__ << ": " #cond "\n";   \
        } else {                                                        \
            ++failures;                                                 \
            std::cout << "FAIL line " << __LINE__ << ": " #cond "\n";   \
        }                                                               \
    } while (0)

int main() {
    std::signal(SIGPIPE, SIG_IGN);

    // ---- JSON roundtrip ----
    json::Value v = json::Value::object();
    v["name"] = "agent \"x\"\n\t";
    v["n"] = 42;
    v["big"] = 9007199254740992LL;
    v["pi"] = 3.14159;
    v["ok"] = true;
    v["nothing"] = json::Value();
    json::Value arr = json::Value::array();
    arr.push(1);
    arr.push("two");
    arr.push(3.5);
    v["list"] = arr;
    std::string s = json::dump(v);
    json::Value back = json::parse(s);
    CHECK(back.at("name").as_string() == "agent \"x\"\n\t");
    CHECK(back.at("n").as_number() == 42.0);
    CHECK(back.at("big").as_number() == 9007199254740992.0);
    CHECK(std::fabs(back.at("pi").as_number() - 3.14159) < 1e-9);
    CHECK(back.at("ok").as_bool(true));
    CHECK(back.at("nothing").is_null());
    CHECK(back.at("list").arr.size() == 3);
    CHECK(back.at("list").arr[1].as_string() == "two");

    bool threw = false;
    try { (void)json::parse("{bad json"); } catch (const json::parse_error&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { (void)json::parse("[1,2] trailing"); } catch (const json::parse_error&) { threw = true; }
    CHECK(threw);

    // ---- get_by_path ----
    json::Value m = json::Value::object();
    m["memory"]["percent"] = 55.5;
    const json::Value* p = get_by_path(m, "memory.percent");
    CHECK(p != nullptr && p->as_number() == 55.5);
    CHECK(get_by_path(m, "memory.nope") == nullptr);
    CHECK(get_by_path(m, "") == &m);

    // ---- check_alerts ----
    std::map<std::string, double> th {{"cpu_percent_total", 90.0}, {"memory.percent", 90.0}};
    json::Value met = json::Value::object();
    met["cpu_percent_total"] = 95.0;
    met["memory"]["percent"] = 50.0;
    auto alerts = check_alerts(met, th);
    CHECK(alerts.size() == 1);
    CHECK(alerts[0].at("metric").as_string() == "cpu_percent_total");
    met["cpu_percent_total"] = 10.0;
    CHECK(check_alerts(met, th).empty());

    // ---- collect_metrics smoke test ----
    json::Value cm = collect_metrics();
    CHECK(cm.at("type").as_string() == "metrics");
    CHECK(get_by_path(cm, "memory.percent") != nullptr);
    CHECK(get_by_path(cm, "disk_root.percent") != nullptr);
    CHECK(get_by_path(cm, "cpu_percent_total") != nullptr);
    json::Value info = full_system_info();
    CHECK(info.at("platform_info").at("system").as_string() == "Linux");

    // ---- LineConnection over socketpair: normal roundtrip + EOF ----
    int fds[2];
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    {
        LineConnection a(fds[0]), b(fds[1]);
        json::Value m1 = json::Value::object();
        m1["type"] = "hello";
        m1["n"] = 1;
        json::Value m2 = json::Value::object();
        m2["type"] = "bye";
        a.send(m1);
        a.send(m2);
        json::Value r1, r2;
        CHECK(b.recv(r1) && r1.at("type").as_string() == "hello" && r1.at("n").as_number() == 1.0);
        CHECK(b.recv(r2) && r2.at("type").as_string() == "bye");
        a.close();
        json::Value r3;
        CHECK(!b.recv(r3));  // clean EOF -> false
    }

    // ---- non-object JSON message is rejected ----
    {
        int f2[2];
        CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, f2) == 0);
        LineConnection b(f2[1]);
        (void)::send(f2[0], "[1,2,3]\n", 8, MSG_NOSIGNAL);
        json::Value dummy;
        bool threw2 = false;
        try { (void)b.recv(dummy); } catch (const ProtocolError&) { threw2 = true; }
        CHECK(threw2);
        ::close(f2[0]);
        b.close();
    }

    // ---- oversized incoming message is rejected (> 4 MiB cap) ----
    {
        int f3[2];
        CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, f3) == 0);
        LineConnection b(f3[1]);
        std::string raw(5 * 1024 * 1024, 'A');
        raw += '\n';
        std::thread writer([&] {
            size_t off = 0;
            while (off < raw.size()) {
                ssize_t n = ::send(f3[0], raw.data() + off, raw.size() - off, MSG_NOSIGNAL);
                if (n <= 0) break;
                off += static_cast<size_t>(n);
            }
        });
        json::Value dummy;
        bool threw3 = false;
        try { (void)b.recv(dummy); } catch (const ProtocolError&) { threw3 = true; }
        CHECK(threw3);
        b.close();
        writer.join();
        ::close(f3[0]);
    }

    // ---- oversized outgoing message is rejected ----
    {
        int f4[2];
        CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, f4) == 0);
        LineConnection a(f4[0]);
        json::Value big = json::Value::object();
        big["pad"] = std::string(5 * 1024 * 1024, 'A');
        bool threw4 = false;
        try { a.send(big); } catch (const ProtocolError&) { threw4 = true; }
        CHECK(threw4);
        ::close(f4[1]);
        a.close();
    }

    std::cout << (failures == 0 ? "\nALL TESTS PASSED\n" : "\nSOME TESTS FAILED\n");
    return failures == 0 ? 0 : 1;
}
