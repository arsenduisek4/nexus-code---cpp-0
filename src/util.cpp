#include "util.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <ctime>
#include <algorithm>

namespace nexus::util {

std::string home_dir() {
    const char* h = std::getenv("HOME");
    // если HOME пустой — значит мы где-то в жопе (cron, systemd unit без env).
    // не падаем, кидаем "." и молимся
    return h ? std::string(h) : std::string(".");
}

std::string expand_path(const std::string& path) {
    if (path.empty()) return path;
    // тильду руками разворачиваем, потому что шелл сюда не лезет и ~ остаётся ~
    if (path[0] == '~') return home_dir() + path.substr(1);
    return path;
}

std::string read_file_raw(const std::string& path, bool& ok) {
    std::ifstream f(expand_path(path), std::ios::binary);
    if (!f) { ok = false; return {}; }
    std::ostringstream ss;
    ss << f.rdbuf();
    ok = true;
    return ss.str();
}

bool write_file_raw(const std::string& path, const std::string& data) {
    std::ofstream f(expand_path(path), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    return f.good();
}

bool file_exists(const std::string& path) {
    struct stat st{};
    return stat(expand_path(path).c_str(), &st) == 0;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};   // строка из одних пробелов, нахуй её
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream ss(s);
    while (std::getline(ss, cur, delim)) out.push_back(cur);
    return out;
}

std::string join(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

std::string now_iso() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    // localtime не потокобезопасный, но у нас один поток, так что похуй
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", std::localtime(&t));
    return buf;
}

std::string shell_escape(const std::string& arg) {
    // оборачиваем в одинарные кавычки и экранируем сами кавычки магией '\''
    // выглядит как ёбаный иероглиф, но это канонический способ не словить инъекцию
    std::string out = "'";
    for (char c : arg) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

std::string to_lower(const std::string& s) {
    std::string out = s;
    // каст к unsigned char обязателен, иначе на отрицательных char-ах tolower даёт UB.
    // да, в C++ это реально так, я охуел когда узнал
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

}
