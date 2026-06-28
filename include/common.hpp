#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>

namespace nexus {

using json = nlohmann::json;

namespace color {
inline const char* reset  = "\033[0m";
inline const char* bold   = "\033[1m";
inline const char* dim    = "\033[2m";
inline const char* red    = "\033[31m";
inline const char* green  = "\033[32m";
inline const char* yellow = "\033[33m";
inline const char* blue   = "\033[34m";
inline const char* magenta= "\033[35m";
inline const char* cyan   = "\033[36m";
inline const char* gray   = "\033[90m";
}

}
