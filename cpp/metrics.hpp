// Linux system metrics collected from /proc and syscalls.
#pragma once

#include "json.hpp"

#include <string>

// One heartbeat sample. CPU percentages are computed against the previous
// call (the first call returns the since-boot average).
json::Value collect_metrics();

// Detailed read-only inventory, used by "get_system_info" / "get <dot.path>".
json::Value full_system_info();

std::string system_hostname();
