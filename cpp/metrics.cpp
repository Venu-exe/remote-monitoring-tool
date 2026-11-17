#include "metrics.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr const char* AGENT_VERSION = "3.0.0-cpp";

std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

double now_seconds() { return static_cast<double>(std::time(nullptr)); }

// ---------------- CPU ----------------
struct CpuSample {
    double total = 0.0;
    double idle = 0.0;  // idle + iowait
};

bool read_cpu(std::vector<CpuSample>& cores, CpuSample& agg) {
    std::ifstream f("/proc/stat");
    if (!f) return false;
    cores.clear();
    bool have_agg = false;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("cpu", 0) != 0) continue;
        std::istringstream ss(line);
        std::string label;
        ss >> label;
        double total = 0.0, idle = 0.0, v = 0.0;
        int field = 0;
        while (ss >> v) {
            ++field;
            total += v;
            if (field == 4 || field == 5) idle += v;  // idle, iowait
        }
        CpuSample s{total, idle};
        if (label == "cpu") {
            agg = s;
            have_agg = true;
        } else {
            cores.push_back(s);
        }
    }
    return have_agg;
}

double cpu_percent(const CpuSample& prev, const CpuSample& cur) {
    double dt = cur.total - prev.total;
    double di = cur.idle - prev.idle;
    if (dt <= 0.0) return 0.0;
    double p = 100.0 * (1.0 - di / dt);
    if (p < 0.0) return 0.0;
    if (p > 100.0) return 100.0;
    return p;
}

// ---------------- memory ----------------
struct MemInfo {
    long long total = 0, available = 0, swap_total = 0, swap_free = 0;
};

MemInfo read_meminfo() {
    MemInfo m;
    std::ifstream f("/proc/meminfo");
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string key;
        long long val = 0;
        std::string unit;
        if (!(ss >> key >> val >> unit)) continue;
        long long bytes = val * 1024;
        if (key == "MemTotal:") m.total = bytes;
        else if (key == "MemAvailable:") m.available = bytes;
        else if (key == "SwapTotal:") m.swap_total = bytes;
        else if (key == "SwapFree:") m.swap_free = bytes;
    }
    return m;
}

// ---------------- disk ----------------
struct DiskInfo {
    long long total = 0, used = 0, free_bytes = 0;
    double percent = 0.0;
};

DiskInfo read_disk_root() {
    DiskInfo d;
    struct statvfs st {};
    if (::statvfs("/", &st) != 0) return d;
    long long total = static_cast<long long>(st.f_blocks) * st.f_frsize;
    long long freeb = static_cast<long long>(st.f_bfree) * st.f_frsize;
    d.total = total;
    d.free_bytes = freeb;
    d.used = total - freeb;
    if (d.used < 0) d.used = 0;
    d.percent = total > 0 ? 100.0 * static_cast<double>(d.used) / static_cast<double>(total) : 0.0;
    return d;
}

// ---------------- network ----------------
struct NetIo {
    long long bytes_recv = 0, bytes_sent = 0;
};

void read_net_dev(NetIo& agg, std::map<std::string, NetIo>& per_if) {
    std::ifstream f("/proc/net/dev");
    std::string line;
    std::getline(f, line);  // header
    std::getline(f, line);  // header
    while (std::getline(f, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = line.substr(0, colon);
        size_t b = name.find_first_not_of(" \t");
        if (b != std::string::npos) name = name.substr(b);
        std::istringstream ss(line.substr(colon + 1));
        long long vals[16] = {0};
        int i = 0;
        while (i < 16 && ss >> vals[i]) ++i;
        if (i < 9) continue;
        NetIo n{vals[0], vals[8]};
        agg.bytes_recv += n.bytes_recv;
        agg.bytes_sent += n.bytes_sent;
        per_if[name] = n;
    }
}

// ---------------- load / uptime / boot ----------------
double uptime_seconds() {
    std::ifstream f("/proc/uptime");
    double u = 0.0;
    f >> u;
    return u;
}

double boot_time_epoch() {
    std::ifstream f("/proc/stat");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("btime", 0) == 0) {
            std::istringstream ss(line.substr(5));
            double t = 0.0;
            ss >> t;
            return t;
        }
    }
    return 0.0;
}

// ---------------- processes ----------------
bool is_numeric(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

long process_count() {
    DIR* dir = ::opendir("/proc");
    if (!dir) return 0;
    long count = 0;
    while (dirent* e = ::readdir(dir)) {
        if (is_numeric(e->d_name)) ++count;
    }
    ::closedir(dir);
    return count;
}

struct ProcInfo {
    long pid = 0;
    std::string name;
    long long rss = 0;
    double mem_percent = 0.0;
};

std::vector<ProcInfo> top_processes(size_t limit, long long mem_total) {
    static const long page_size = ::sysconf(_SC_PAGESIZE);
    std::vector<ProcInfo> procs;
    DIR* dir = ::opendir("/proc");
    if (!dir) return procs;
    while (dirent* e = ::readdir(dir)) {
        if (!is_numeric(e->d_name)) continue;
        long pid = std::strtol(e->d_name, nullptr, 10);
        std::string statm = read_file("/proc/" + std::to_string(pid) + "/statm");
        if (statm.empty()) continue;
        std::istringstream ss(statm);
        long long size_pages = 0, resident_pages = 0;
        ss >> size_pages >> resident_pages;
        if (ss.fail()) continue;
        std::string comm = read_file("/proc/" + std::to_string(pid) + "/comm");
        if (!comm.empty() && comm.back() == '\n') comm.pop_back();
        if (comm.empty()) comm = "?";
        ProcInfo p;
        p.pid = pid;
        p.name = comm;
        p.rss = resident_pages * page_size;
        p.mem_percent =
            mem_total > 0 ? 100.0 * static_cast<double>(p.rss) / static_cast<double>(mem_total) : 0.0;
        procs.push_back(std::move(p));
    }
    ::closedir(dir);
    std::sort(procs.begin(), procs.end(),
              [](const ProcInfo& a, const ProcInfo& b) { return a.rss > b.rss; });
    if (procs.size() > limit) procs.resize(limit);
    return procs;
}

} // namespace

std::string system_hostname() {
    char buf[256] = {0};
    if (::gethostname(buf, sizeof(buf) - 1) != 0) return "unknown";
    buf[sizeof(buf) - 1] = 0;
    return buf;
}

json::Value collect_metrics() {
    static std::vector<CpuSample> prev_cores;
    static CpuSample prev_agg;
    static bool have_prev = false;

    std::vector<CpuSample> cores;
    CpuSample agg;
    bool ok = read_cpu(cores, agg);
    double total_pct = 0.0;
    json::Value per_core = json::Value::array();
    if (ok) {
        if (have_prev) {
            total_pct = cpu_percent(prev_agg, agg);
            for (size_t i = 0; i < cores.size() && i < prev_cores.size(); ++i)
                per_core.push(cpu_percent(prev_cores[i], cores[i]));
            for (size_t i = prev_cores.size(); i < cores.size(); ++i) per_core.push(0.0);
        } else {
            total_pct = agg.total > 0 ? 100.0 * (1.0 - agg.idle / agg.total) : 0.0;
            for (auto& c : cores)
                per_core.push(c.total > 0 ? 100.0 * (1.0 - c.idle / c.total) : 0.0);
        }
        prev_cores = cores;
        prev_agg = agg;
        have_prev = true;
    }

    MemInfo mem = read_meminfo();
    long long used = mem.total - mem.available;
    if (used < 0) used = 0;
    double mem_pct =
        mem.total > 0 ? 100.0 * static_cast<double>(used) / static_cast<double>(mem.total) : 0.0;
    DiskInfo disk = read_disk_root();
    NetIo net;
    std::map<std::string, NetIo> per_if;
    read_net_dev(net, per_if);

    double load[3] = {0.0, 0.0, 0.0};
    bool have_load = ::getloadavg(load, 3) == 3;

    json::Value m = json::Value::object();
    m["type"] = "metrics";
    m["timestamp"] = now_seconds();
    m["hostname"] = system_hostname();
    m["agent_version"] = AGENT_VERSION;
    m["uptime_seconds"] = uptime_seconds();
    m["cpu_percent_total"] = total_pct;
    m["cpu_percent_per_core"] = per_core;
    json::Value la = json::Value::array();
    if (have_load) {
        la.push(load[0]);
        la.push(load[1]);
        la.push(load[2]);
    }
    m["load_avg"] = la;

    json::Value memv = json::Value::object();
    memv["total"] = static_cast<double>(mem.total);
    memv["available"] = static_cast<double>(mem.available);
    memv["used"] = static_cast<double>(used);
    memv["percent"] = mem_pct;
    memv["swap_total"] = static_cast<double>(mem.swap_total);
    memv["swap_free"] = static_cast<double>(mem.swap_free);
    m["memory"] = memv;

    json::Value dv = json::Value::object();
    dv["total"] = static_cast<double>(disk.total);
    dv["used"] = static_cast<double>(disk.used);
    dv["free"] = static_cast<double>(disk.free_bytes);
    dv["percent"] = disk.percent;
    m["disk_root"] = dv;

    json::Value nv = json::Value::object();
    nv["bytes_recv"] = static_cast<double>(net.bytes_recv);
    nv["bytes_sent"] = static_cast<double>(net.bytes_sent);
    m["net_io"] = nv;

    m["process_count"] = static_cast<double>(process_count());

    json::Value tp = json::Value::array();
    for (auto& p : top_processes(5, mem.total)) {
        json::Value pv = json::Value::object();
        pv["pid"] = static_cast<double>(p.pid);
        pv["name"] = p.name;
        pv["memory_percent"] = p.mem_percent;
        tp.push(pv);
    }
    m["top_processes"] = tp;
    return m;
}

json::Value full_system_info() {
    struct utsname un {};
    ::uname(&un);
    json::Value root = json::Value::object();

    json::Value p = json::Value::object();
    p["system"] = un.sysname;
    p["node"] = un.nodename;
    p["release"] = un.release;
    p["version"] = un.version;
    p["machine"] = un.machine;
    double btime = boot_time_epoch();
    if (btime > 0) {
        std::time_t t = static_cast<std::time_t>(btime);
        char buf[64] = {0};
        ::ctime_r(&t, buf);
        std::string s(buf);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        p["boot_time"] = s;
    }
    root["platform_info"] = p;

    json::Value c = json::Value::object();
    c["logical_cores"] = static_cast<double>(::sysconf(_SC_NPROCESSORS_ONLN));
    long physical = 0;
    double mhz = 0.0;
    {
        std::ifstream f("/proc/cpuinfo");
        std::string line;
        while (std::getline(f, line)) {
            if (line.rfind("processor", 0) == 0 && line.find(':') != std::string::npos) ++physical;
            if (mhz == 0.0 && line.rfind("cpu MHz", 0) == 0) {
                auto colon = line.find(':');
                if (colon != std::string::npos) mhz = std::stod(line.substr(colon + 1));
            }
        }
    }
    c["physical_cores"] = static_cast<double>(physical);
    c["freq_mhz"] = mhz;
    root["cpu_info"] = c;

    MemInfo mem = read_meminfo();
    long long used = mem.total - mem.available;
    if (used < 0) used = 0;
    json::Value vm = json::Value::object();
    vm["total"] = static_cast<double>(mem.total);
    vm["available"] = static_cast<double>(mem.available);
    vm["used"] = static_cast<double>(used);
    vm["percent"] =
        mem.total > 0 ? 100.0 * static_cast<double>(used) / static_cast<double>(mem.total) : 0.0;
    json::Value sm = json::Value::object();
    sm["total"] = static_cast<double>(mem.swap_total);
    sm["free"] = static_cast<double>(mem.swap_free);
    sm["used"] = static_cast<double>(mem.swap_total - mem.swap_free);
    json::Value mi = json::Value::object();
    mi["virtual_memory"] = vm;
    mi["swap_memory"] = sm;
    root["memory_info"] = mi;

    DiskInfo disk = read_disk_root();
    json::Value du = json::Value::object();
    du["total"] = static_cast<double>(disk.total);
    du["used"] = static_cast<double>(disk.used);
    du["free"] = static_cast<double>(disk.free_bytes);
    du["percent"] = disk.percent;
    json::Value di = json::Value::object();
    di["usage_root"] = du;
    root["disk_info"] = di;

    NetIo net;
    std::map<std::string, NetIo> per_if;
    read_net_dev(net, per_if);
    json::Value io = json::Value::object();
    io["bytes_recv"] = static_cast<double>(net.bytes_recv);
    io["bytes_sent"] = static_cast<double>(net.bytes_sent);
    json::Value ifaces = json::Value::object();
    for (auto& kv : per_if) {
        json::Value n = json::Value::object();
        n["bytes_recv"] = static_cast<double>(kv.second.bytes_recv);
        n["bytes_sent"] = static_cast<double>(kv.second.bytes_sent);
        ifaces[kv.first] = n;
    }
    json::Value ni = json::Value::object();
    ni["io_counters"] = io;
    ni["interfaces"] = ifaces;
    root["network_info"] = ni;

    return root;
}
