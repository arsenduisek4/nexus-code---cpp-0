#include "tools.hpp"
#include "config.hpp"
#include "history.hpp"
#include "memory.hpp"
#include "http.hpp"
#include "util.hpp"
#include "logger.hpp"

#include <cstdio>
#include <cstdlib>
#include <array>
#include <sstream>
#include <fstream>
#include <random>
#include <ctime>
#include <cmath>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace nexus {

namespace {

// рабочая лошадка всех тулзов: запустить шелл-команду и забрать stdout+stderr вместе.
// 2>&1 приклеиваем всегда — модели полезно видеть и ошибки, а не только успешный вывод
std::pair<int, std::string> sh(const std::string& cmd) {
    FILE* p = popen((cmd + " 2>&1").c_str(), "r");
    if (!p) return {-1, "failed to start command"};
    std::string out;
    std::array<char, 4096> buf{};
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), p)) > 0) out.append(buf.data(), n);
    int rc = pclose(p);
    int code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
    return {code, out};
}

std::string clip(const std::string& s, size_t max = 4000000) {
    if (s.size() <= max) return s;
    return s.substr(0, max) + "\n...[truncated " + std::to_string(s.size() - max) + " bytes]";
}

std::string sarg(const json& a, const char* k, const std::string& def = "") {
    if (!a.is_object() || !a.contains(k) || a[k].is_null()) return def;
    if (a[k].is_string()) return a[k].get<std::string>();
    return a[k].dump();
}

long iarg(const json& a, const char* k, long def = 0) {
    if (!a.is_object() || !a.contains(k) || a[k].is_null()) return def;
    if (a[k].is_number()) return a[k].get<long>();
    if (a[k].is_string()) { try { return std::stol(a[k].get<std::string>()); } catch (...) {} }
    return def;
}

bool has_cmd(const std::string& name) {
    return sh("command -v " + util::shell_escape(name)).first == 0;
}

struct P { std::string name, type, desc; bool req; };
json schema(std::initializer_list<P> ps) {
    json properties = json::object();
    json required = json::array();
    for (const auto& p : ps) {
        properties[p.name] = {{"type", p.type}, {"description", p.desc}};
        if (p.req) required.push_back(p.name);
    }
    return {{"type", "object"}, {"properties", properties}, {"required", required}};
}

// микро-калькулятор рекурсивным спуском (+ - * / % ^ и скобки). написал сам, потому что
// тащить eval() или libmatheval ради «посчитай 2+2» — это блядский оверкилл и дыра в безопасности
struct Calc {
    const std::string& s;
    size_t i = 0;
    bool err = false;
    explicit Calc(const std::string& str) : s(str) {}
    void ws() { while (i < s.size() && std::isspace((unsigned char)s[i])) ++i; }
    double parse() { double v = expr(); ws(); if (i != s.size()) err = true; return v; }
    double expr() {
        double v = term();
        for (;;) { ws();
            if (i < s.size() && s[i] == '+') { ++i; v += term(); }
            else if (i < s.size() && s[i] == '-') { ++i; v -= term(); }
            else break;
        }
        return v;
    }
    double term() {
        double v = power();
        for (;;) { ws();
            if (i < s.size() && s[i] == '*') { ++i; v *= power(); }
            else if (i < s.size() && s[i] == '/') { ++i; double d = power(); v = d ? v / d : (err = true, 0); }
            else if (i < s.size() && s[i] == '%') { ++i; double d = power(); v = d ? std::fmod(v, d) : (err = true, 0); }
            else break;
        }
        return v;
    }
    double power() {
        double v = factor();
        ws();
        if (i < s.size() && s[i] == '^') { ++i; v = std::pow(v, power()); }
        return v;
    }
    double factor() {
        ws();
        if (i < s.size() && s[i] == '(') { ++i; double v = expr(); ws(); if (i < s.size() && s[i] == ')') ++i; else err = true; return v; }
        if (i < s.size() && (s[i] == '-' )) { ++i; return -factor(); }
        if (i < s.size() && (s[i] == '+')) { ++i; return factor(); }
        size_t start = i;
        while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '.')) ++i;
        if (i == start) { err = true; return 0; }
        return std::strtod(s.c_str() + start, nullptr);
    }
};

} // namespace

void ToolRegistry::add(Tool t) { tools_[t.name] = std::move(t); }

const Tool* ToolRegistry::find(const std::string& name) const {
    auto it = tools_.find(name);
    return it == tools_.end() ? nullptr : &it->second;
}

std::vector<std::string> ToolRegistry::names() const {
    std::vector<std::string> out;
    for (auto& [k, v] : tools_) out.push_back(k);
    return out;
}

std::map<std::string, std::vector<std::string>> ToolRegistry::by_category() const {
    std::map<std::string, std::vector<std::string>> out;
    for (auto& [k, v] : tools_) out[v.category].push_back(k);
    return out;
}

json ToolRegistry::schemas() const {
    json arr = json::array();
    for (auto& [k, v] : tools_) {
        if (!v.exposed) continue;
        arr.push_back({
            {"type", "function"},
            {"function", {
                {"name", v.name},
                {"description", v.description},
                {"parameters", v.parameters},
            }},
        });
    }
    return arr;
}

std::string ToolRegistry::call(const std::string& name, const json& args, ToolContext& ctx) const {
    const Tool* t = find(name);
    if (!t) return "ошибка: неизвестный инструмент '" + name + "'";
    Logger::instance().info("tool " + name + " " + args.dump());
    try {
        return t->run(args, ctx);
    } catch (const std::exception& e) {
        return std::string("ошибка: ") + e.what();
    }
}

void ToolRegistry::register_all() {
    // ---------- 1. Files ----------
    add({"read_file", "Read a text file, optionally a line range.", "files",
         schema({{"path","string","File path",true},
                 {"offset","integer","Start line (1-based)",false},
                 {"limit","integer","Max lines",false}}), true, false,
        [](const json& a, ToolContext&) -> std::string {
            bool ok = false;
            std::string data = util::read_file_raw(sarg(a, "path"), ok);
            if (!ok) return "ошибка: не удаётся прочитать " + sarg(a, "path");
            long off = iarg(a, "offset", 0), lim = iarg(a, "limit", 0);
            if (off <= 0 && lim <= 0) return clip(data);
            auto lines = util::split(data, '\n');
            std::string out;
            long start = off > 0 ? off - 1 : 0;
            long end = lim > 0 ? start + lim : (long)lines.size();
            for (long i = start; i < end && i < (long)lines.size(); ++i)
                out += lines[i] + "\n";
            return clip(out);
        }});

    add({"write_file", "Create or overwrite a file with content.", "files",
         schema({{"path","string","File path",true},{"content","string","File content",true}}),
         true, true,
        [](const json& a, ToolContext&) -> std::string {
            if (!util::write_file_raw(sarg(a, "path"), sarg(a, "content")))
                return "ошибка: не удалось записать";
            return "записано " + std::to_string(sarg(a, "content").size()) + " байт в " + sarg(a, "path");
        }});

    add({"edit_file", "Replace the first occurrence of old_string with new_string in a file.", "files",
         schema({{"path","string","File path",true},
                 {"old_string","string","Text to find",true},
                 {"new_string","string","Replacement text",true}}), true, true,
        [](const json& a, ToolContext&) -> std::string {
            bool ok = false;
            std::string data = util::read_file_raw(sarg(a, "path"), ok);
            if (!ok) return "ошибка: не удаётся прочитать " + sarg(a, "path");
            std::string olds = sarg(a, "old_string");
            auto pos = data.find(olds);
            if (pos == std::string::npos) return "ошибка: old_string не найден";
            data.replace(pos, olds.size(), sarg(a, "new_string"));
            if (!util::write_file_raw(sarg(a, "path"), data)) return "ошибка: не удалось записать";
            return "изменён " + sarg(a, "path");
        }});

    add({"append_file", "Append content to the end of a file.", "files",
         schema({{"path","string","File path",true},{"content","string","Text to append",true}}),
         true, true,
        [](const json& a, ToolContext&) -> std::string {
            std::ofstream f(util::expand_path(sarg(a, "path")), std::ios::app);
            if (!f) return "ошибка: не удаётся открыть " + sarg(a, "path");
            f << sarg(a, "content");
            return "добавлено в " + sarg(a, "path");
        }});

    add({"delete_file", "Delete a file.", "files",
         schema({{"path","string","File path",true}}), true, true,
        [](const json& a, ToolContext&) -> std::string {
            return std::remove(util::expand_path(sarg(a, "path")).c_str()) == 0
                ? "удалён " + sarg(a, "path") : "ошибка: не удалось удалить";
        }});

    add({"rename_file", "Rename a file or directory.", "files",
         schema({{"from","string","Current path",true},{"to","string","New path",true}}), true, true,
        [](const json& a, ToolContext&) -> std::string {
            return std::rename(util::expand_path(sarg(a, "from")).c_str(),
                               util::expand_path(sarg(a, "to")).c_str()) == 0
                ? "переименовано" : "ошибка: не удалось переименовать";
        }});

    add({"copy_file", "Copy a file or directory.", "files",
         schema({{"from","string","Source",true},{"to","string","Destination",true}}), true, true,
        [](const json& a, ToolContext&) -> std::string {
            auto r = sh("cp -r " + util::shell_escape(util::expand_path(sarg(a, "from"))) + " " +
                        util::shell_escape(util::expand_path(sarg(a, "to"))));
            return r.first == 0 ? "скопировано" : "ошибка: " + r.second;
        }});

    add({"move_file", "Move a file or directory.", "files",
         schema({{"from","string","Source",true},{"to","string","Destination",true}}), true, true,
        [](const json& a, ToolContext&) -> std::string {
            auto r = sh("mv " + util::shell_escape(util::expand_path(sarg(a, "from"))) + " " +
                        util::shell_escape(util::expand_path(sarg(a, "to"))));
            return r.first == 0 ? "перемещено" : "ошибка: " + r.second;
        }});

    add({"list_dir", "List the contents of a directory.", "files",
         schema({{"path","string","Directory (default .)",false}}), true, false,
        [](const json& a, ToolContext&) -> std::string {
            std::string p = sarg(a, "path", ".");
            auto r = sh("ls -la --group-directories-first " + util::shell_escape(util::expand_path(p)));
            return clip(r.second);
        }});

    add({"tree", "Show a directory tree.", "files",
         schema({{"path","string","Directory (default .)",false},
                 {"depth","integer","Max depth (default 3)",false}}), true, false,
        [](const json& a, ToolContext&) -> std::string {
            std::string p = util::expand_path(sarg(a, "path", "."));
            long d = iarg(a, "depth", 3);
            std::string cmd = has_cmd("tree")
                ? "tree -L " + std::to_string(d) + " " + util::shell_escape(p)
                : "find " + util::shell_escape(p) + " -maxdepth " + std::to_string(d);
            return clip(sh(cmd).second);
        }});

    // ---------- 2. Search ----------
    auto grep_impl = [](const json& a, ToolContext&) -> std::string {
        std::string pat = sarg(a, "pattern");
        std::string path = util::expand_path(sarg(a, "path", "."));
        std::string cmd = "grep -rnI --color=never " + util::shell_escape(pat) + " " +
                          util::shell_escape(path);
        auto r = sh(cmd);
        if (r.second.empty()) return "нет совпадений";
        return clip(r.second);
    };
    add({"grep", "Search file contents for a regex pattern.", "search",
         schema({{"pattern","string","Regex pattern",true},{"path","string","Path (default .)",false}}),
         true, false, grep_impl});

    add({"find_file", "Find files by name pattern.", "search",
         schema({{"name","string","Filename glob, e.g. *.cpp",true},{"path","string","Root (default .)",false}}),
         true, false,
        [](const json& a, ToolContext&) -> std::string {
            std::string cmd = "find " + util::shell_escape(util::expand_path(sarg(a, "path", "."))) +
                              " -name " + util::shell_escape(sarg(a, "name"));
            auto r = sh(cmd);
            return r.second.empty() ? "файлы не найдены" : clip(r.second);
        }});

    add({"search_code", "Search source code for a pattern (code files only).", "search",
         schema({{"pattern","string","Regex pattern",true},{"path","string","Root (default .)",false}}),
         true, false,
        [](const json& a, ToolContext&) -> std::string {
            std::string cmd = "grep -rnI --include='*.c' --include='*.cpp' --include='*.h' "
                              "--include='*.hpp' --include='*.py' --include='*.js' --include='*.ts' "
                              "--include='*.go' --include='*.rs' --include='*.java' " +
                              util::shell_escape(sarg(a, "pattern")) + " " +
                              util::shell_escape(util::expand_path(sarg(a, "path", ".")));
            auto r = sh(cmd);
            return r.second.empty() ? "нет совпадений" : clip(r.second);
        }});

    add({"search_web", "Search the web (DuckDuckGo instant answers).", "search",
         schema({{"query","string","Search query",true}}), true, false,
        [](const json& a, ToolContext&) -> std::string {
            std::string q = sarg(a, "query");
            std::string url = "https://api.duckduckgo.com/?format=json&no_html=1&q=";
            for (char c : q) {
                if (std::isalnum((unsigned char)c)) url += c;
                else { char b[4]; std::snprintf(b, sizeof b, "%%%02X", (unsigned char)c); url += b; }
            }
            auto r = http::get(url);
            if (!r.ok()) return "ошибка: " + (r.error.empty() ? "http " + std::to_string(r.status) : r.error);
            try {
                json j = json::parse(r.body);
                std::string out;
                if (j.contains("AbstractText") && !j["AbstractText"].get<std::string>().empty())
                    out += j["AbstractText"].get<std::string>() + "\n";
                if (j.contains("RelatedTopics"))
                    for (auto& t : j["RelatedTopics"])
                        if (t.contains("Text")) out += "- " + t["Text"].get<std::string>() + "\n";
                return out.empty() ? "нет краткого ответа" : clip(out);
            } catch (...) { return clip(r.body); }
        }});

    add({"search_project", "Grep across the current project directory.", "search",
         schema({{"pattern","string","Regex pattern",true}}), true, false, grep_impl});

    // ---------- 3. Bash / system ----------
    add({"run_bash", "Run a bash command and return combined stdout/stderr.", "system",
         schema({{"command","string","Shell command",true},
                 {"timeout","integer","Seconds (default 60)",false}}), true, true,
        [](const json& a, ToolContext&) -> std::string {
            long to = iarg(a, "timeout", 60);
            std::string cmd = sarg(a, "command");
            if (has_cmd("timeout")) cmd = "timeout " + std::to_string(to) + " bash -c " + util::shell_escape(cmd);
            auto r = sh(cmd);
            return clip("[выход " + std::to_string(r.first) + "]\n" + r.second);
        }});

    add({"run_script", "Execute a script file.", "system",
         schema({{"path","string","Script path",true},{"args","string","Arguments",false}}), true, true,
        [](const json& a, ToolContext&) -> std::string {
            auto r = sh("bash " + util::shell_escape(util::expand_path(sarg(a, "path"))) + " " + sarg(a, "args"));
            return clip("[выход " + std::to_string(r.first) + "]\n" + r.second);
        }});

    add({"kill_process", "Kill a process by PID.", "system",
         schema({{"pid","integer","Process ID",true},{"signal","string","Signal (default TERM)",false}}),
         true, true,
        [](const json& a, ToolContext&) -> std::string {
            auto r = sh("kill -" + sarg(a, "signal", "TERM") + " " + std::to_string(iarg(a, "pid")));
            return r.first == 0 ? "сигнал отправлен" : "ошибка: " + r.second;
        }});

    add({"list_processes", "List top processes by CPU.", "system",
         schema({}), true, false,
        [](const json&, ToolContext&) -> std::string {
            return clip(sh("ps -eo pid,pcpu,pmem,comm --sort=-pcpu | head -20").second);
        }});

    add({"system_info", "Show OS and host information.", "system", schema({}), true, false,
        [](const json&, ToolContext&) -> std::string {
            return clip(sh("uname -a; echo; cat /etc/os-release 2>/dev/null | head -4; echo; uptime").second);
        }});

    add({"disk_usage", "Show disk usage.", "system", schema({}), true, false,
        [](const json&, ToolContext&) -> std::string { return clip(sh("df -h").second); }});

    add({"memory_usage", "Show RAM usage.", "system", schema({}), true, false,
        [](const json&, ToolContext&) -> std::string { return clip(sh("free -h").second); }});

    add({"cpu_usage", "Show CPU load average and core count.", "system", schema({}), true, false,
        [](const json&, ToolContext&) -> std::string {
            return clip(sh("cat /proc/loadavg; echo; nproc --all 2>/dev/null").second);
        }});

    add({"network_info", "Show network interfaces and addresses.", "system", schema({}), true, false,
        [](const json&, ToolContext&) -> std::string {
            return clip(sh(has_cmd("ip") ? "ip -brief addr" : "ifconfig").second);
        }});

    add({"ping", "Ping a host.", "system",
         schema({{"host","string","Host or IP",true},{"count","integer","Packets (default 4)",false}}),
         true, false,
        [](const json& a, ToolContext&) -> std::string {
            return clip(sh("ping -c " + std::to_string(iarg(a, "count", 4)) + " " +
                           util::shell_escape(sarg(a, "host"))).second);
        }});

    // ---------- 4. Servers ----------
    auto srv_pidfile = [](const std::string& name) {
        return std::string("/tmp/nexus_srv_") + name + ".pid";
    };
    auto srv_cmdfile = [](const std::string& name) {
        return std::string("/tmp/nexus_srv_") + name + ".cmd";
    };
    add({"start_server", "Start a long-running command in the background.", "servers",
         schema({{"name","string","Server name",true},{"command","string","Command to run",true}}),
         true, true,
        [srv_pidfile, srv_cmdfile](const json& a, ToolContext&) -> std::string {
            std::string name = sarg(a, "name"), cmd = sarg(a, "command");
            util::write_file_raw(srv_cmdfile(name), cmd);   // команду сохраняем, чтоб потом restart смог её поднять
            std::string log = "/tmp/nexus_srv_" + name + ".log";
            // nohup + & + echo $! — запускаем в фоне, отвязываем от нашего процесса
            // и сразу выгребаем pid дочки. весь стейт серверов живёт в /tmp файликах, без БД
            std::string full = "nohup bash -c " + util::shell_escape(cmd) +
                               " >" + log + " 2>&1 & echo $!";
            auto r = sh(full);
            std::string pid = util::trim(r.second);
            util::write_file_raw(srv_pidfile(name), pid);
            return "запущен '" + name + "' pid=" + pid + " (log: " + log + ")";
        }});

    add({"stop_server", "Stop a background server by name.", "servers",
         schema({{"name","string","Server name",true}}), true, true,
        [srv_pidfile](const json& a, ToolContext&) -> std::string {
            bool ok = false;
            std::string pid = util::trim(util::read_file_raw(srv_pidfile(sarg(a, "name")), ok));
            if (!ok || pid.empty()) return "ошибка: такого сервера нет";
            sh("kill " + pid);
            std::remove(srv_pidfile(sarg(a, "name")).c_str());
            return "остановлен '" + sarg(a, "name") + "' (pid " + pid + ")";
        }});

    add({"check_server", "Check whether a named server is running.", "servers",
         schema({{"name","string","Server name",true}}), true, false,
        [srv_pidfile](const json& a, ToolContext&) -> std::string {
            bool ok = false;
            std::string pid = util::trim(util::read_file_raw(srv_pidfile(sarg(a, "name")), ok));
            if (!ok || pid.empty()) return "не зарегистрирован";
            return sh("kill -0 " + pid).first == 0 ? "работает (pid " + pid + ")" : "остановлен";
        }});

    add({"list_servers", "List registered background servers.", "servers", schema({}), true, false,
        [](const json&, ToolContext&) -> std::string {
            return clip(sh("for f in /tmp/nexus_srv_*.pid; do "
                           "[ -e \"$f\" ] || continue; n=$(basename $f .pid); "
                           "n=${n#nexus_srv_}; p=$(cat $f); "
                           "kill -0 $p 2>/dev/null && s=up || s=down; "
                           "echo \"$n pid=$p $s\"; done").second);
        }});

    add({"restart_server", "Restart a named server using its stored command.", "servers",
         schema({{"name","string","Server name",true}}), true, true,
        [srv_pidfile, srv_cmdfile](const json& a, ToolContext& ctx) -> std::string {
            std::string name = sarg(a, "name");
            bool ok = false;
            std::string cmd = util::trim(util::read_file_raw(srv_cmdfile(name), ok));
            if (!ok) return "ошибка: нет сохранённой команды для '" + name + "'";
            bool pok = false;
            std::string pid = util::trim(util::read_file_raw(srv_pidfile(name), pok));
            if (pok && !pid.empty()) sh("kill " + pid);
            std::string log = "/tmp/nexus_srv_" + name + ".log";
            auto r = sh("nohup bash -c " + util::shell_escape(cmd) + " >" + log + " 2>&1 & echo $!");
            std::string np = util::trim(r.second);
            util::write_file_raw(srv_pidfile(name), np);
            return "перезапущен '" + name + "' pid=" + np;
        }});

    // ---------- 5. API / network ----------
    add({"fetch_url", "Fetch the body of a URL (GET).", "network",
         schema({{"url","string","URL",true}}), true, false,
        [](const json& a, ToolContext&) -> std::string {
            auto r = http::get(sarg(a, "url"));
            if (!r.ok()) return "ошибка: " + (r.error.empty() ? "http " + std::to_string(r.status) : r.error);
            return clip(r.body);
        }});

    add({"download_file", "Download a URL to a local path.", "network",
         schema({{"url","string","URL",true},{"dest","string","Destination path",true}}), true, true,
        [](const json& a, ToolContext&) -> std::string {
            return http::download(sarg(a, "url"), sarg(a, "dest"))
                ? "скачано в " + sarg(a, "dest") : "ошибка: не удалось скачать";
        }});

    add({"post_url", "Send a JSON POST request.", "network",
         schema({{"url","string","URL",true},{"body","string","Request body (JSON string)",true}}),
         true, true,
        [](const json& a, ToolContext&) -> std::string {
            auto r = http::post_json(sarg(a, "url"), {}, sarg(a, "body"));
            if (!r.error.empty()) return "ошибка: " + r.error;
            return clip("[http " + std::to_string(r.status) + "]\n" + r.body);
        }});

    add({"check_connection", "Check internet connectivity to a host.", "network",
         schema({{"host","string","Host (default 1.1.1.1)",false}}), true, false,
        [](const json& a, ToolContext&) -> std::string {
            std::string h = sarg(a, "host", "1.1.1.1");
            return sh("ping -c 1 -W 3 " + util::shell_escape(h)).first == 0 ? "доступен" : "недоступен";
        }});

    add({"dns_lookup", "Resolve a hostname to IP addresses.", "network",
         schema({{"host","string","Hostname",true}}), true, false,
        [](const json& a, ToolContext&) -> std::string {
            std::string cmd = has_cmd("getent")
                ? "getent hosts " + util::shell_escape(sarg(a, "host"))
                : "nslookup " + util::shell_escape(sarg(a, "host"));
            auto r = sh(cmd);
            return r.second.empty() ? "записей нет" : clip(r.second);
        }});

    // ---------- 6. Memory ----------
    add({"memory_add", "Store a fact in long-term vector memory.", "memory",
         schema({{"text","string","Fact to remember",true}}), true, false,
        [](const json& a, ToolContext& ctx) -> std::string {
            auto id = ctx.memory.add(sarg(a, "text"));
            return "запомнено #" + std::to_string(id);
        }});

    add({"memory_search", "Search vector memory for relevant facts.", "memory",
         schema({{"query","string","Search query",true},{"top_k","integer","Results (default 5)",false}}),
         true, false,
        [](const json& a, ToolContext& ctx) -> std::string {
            auto hits = ctx.memory.search(sarg(a, "query"), (int)iarg(a, "top_k", 5));
            if (hits.empty()) return "нет релевантных записей";
            std::string out;
            for (auto& h : hits) {
                char b[16]; std::snprintf(b, sizeof b, "%.2f", h.score);
                out += "[" + std::string(b) + "] " + h.text + "\n";
            }
            return out;
        }});

    add({"memory_list", "List recent memories.", "memory",
         schema({{"limit","integer","Max items (default 20)",false}}), true, false,
        [](const json& a, ToolContext& ctx) -> std::string {
            auto items = ctx.memory.list((int)iarg(a, "limit", 20));
            if (items.empty()) return "память пуста";
            std::string out;
            for (auto& [id, t] : items) out += "#" + std::to_string(id) + " " + t + "\n";
            return out;
        }});

    add({"memory_clear", "Erase all stored memories.", "memory", schema({}), true, true,
        [](const json&, ToolContext& ctx) -> std::string { ctx.memory.clear(); return "память очищена"; }});

    add({"memory_stats", "Show memory statistics.", "memory", schema({}), true, false,
        [](const json&, ToolContext& ctx) -> std::string {
            return std::to_string(ctx.memory.count()) + " memories stored";
        }});

    // ---------- 7. Git ----------
    add({"git_status", "Show git status.", "git", schema({}), true, false,
        [](const json&, ToolContext&) -> std::string { return clip(sh("git status -sb").second); }});

    add({"git_commit", "Stage all changes and create a commit.", "git",
         schema({{"message","string","Commit message",true}}), true, true,
        [](const json& a, ToolContext&) -> std::string {
            sh("git add -A");
            auto r = sh("git commit -m " + util::shell_escape(sarg(a, "message")));
            return clip(r.second);
        }});

    add({"git_push", "Push commits to the remote.", "git",
         schema({{"remote","string","Remote (default origin)",false},{"branch","string","Branch",false}}),
         true, true,
        [](const json& a, ToolContext&) -> std::string {
            return clip(sh("git push " + sarg(a, "remote", "origin") + " " + sarg(a, "branch")).second);
        }});

    add({"git_pull", "Pull from the remote.", "git", schema({}), true, true,
        [](const json&, ToolContext&) -> std::string { return clip(sh("git pull").second); }});

    add({"git_branch", "Create and switch to a new branch.", "git",
         schema({{"name","string","Branch name",true}}), true, true,
        [](const json& a, ToolContext&) -> std::string {
            return clip(sh("git checkout -b " + util::shell_escape(sarg(a, "name"))).second);
        }});

    // ---------- 8. Deploy ----------
    add({"deploy_netlify", "Deploy a folder to Netlify (needs netlify CLI).", "deploy",
         schema({{"dir","string","Publish directory",true}}), true, true,
        [](const json& a, ToolContext&) -> std::string {
            if (!has_cmd("netlify")) return "ошибка: netlify CLI не установлен";
            return clip(sh("netlify deploy --prod --dir " + util::shell_escape(sarg(a, "dir"))).second);
        }});

    add({"deploy_vps", "Rsync a folder to a VPS over SSH.", "deploy",
         schema({{"dir","string","Local dir",true},{"target","string","user@host:/path",true}}), true, true,
        [](const json& a, ToolContext&) -> std::string {
            return clip(sh("rsync -avz --delete " + util::shell_escape(sarg(a, "dir") + "/") + " " +
                           util::shell_escape(sarg(a, "target"))).second);
        }});

    add({"deploy_github", "Push current branch to GitHub.", "deploy",
         schema({{"branch","string","Branch (default main)",false}}), true, true,
        [](const json& a, ToolContext&) -> std::string {
            return clip(sh("git push origin " + sarg(a, "branch", "main")).second);
        }});

    add({"deploy_ssh", "Run a command on a remote host over SSH.", "deploy",
         schema({{"host","string","user@host",true},{"command","string","Remote command",true}}), true, true,
        [](const json& a, ToolContext&) -> std::string {
            return clip(sh("ssh " + util::shell_escape(sarg(a, "host")) + " " +
                           util::shell_escape(sarg(a, "command"))).second);
        }});

    add({"sync_ssh", "Sync a directory to a remote host with rsync.", "deploy",
         schema({{"src","string","Local source",true},{"target","string","user@host:/path",true}}), true, true,
        [](const json& a, ToolContext&) -> std::string {
            return clip(sh("rsync -avz " + util::shell_escape(sarg(a, "src")) + " " +
                           util::shell_escape(sarg(a, "target"))).second);
        }});

    // ---------- 9. Code / development ----------
    add({"run_tests", "Detect and run the project's test suite.", "code", schema({}), true, true,
        [](const json&, ToolContext&) -> std::string {
            std::string cmd;
            if (util::file_exists("package.json")) cmd = "npm test";
            else if (util::file_exists("pytest.ini") || util::file_exists("tests")) cmd = "pytest -q";
            else if (util::file_exists("Cargo.toml")) cmd = "cargo test";
            else if (util::file_exists("CMakeLists.txt")) cmd = "ctest --test-dir build --output-on-failure";
            else return "ошибка: не найдена конфигурация тестов";
            auto r = sh(cmd);
            return clip("[выход " + std::to_string(r.first) + "]\n" + r.second);
        }});

    add({"init_project", "Scaffold a new project (cpp, python, node).", "code",
         schema({{"name","string","Project name",true},{"type","string","cpp|python|node",true}}), true, true,
        [](const json& a, ToolContext&) -> std::string {
            std::string name = sarg(a, "name"), type = util::to_lower(sarg(a, "type"));
            std::string base = util::expand_path(name);
            mkdir(base.c_str(), 0755);
            if (type == "python") {
                util::write_file_raw(base + "/main.py", "def main():\n    print('hello')\n\nif __name__ == '__main__':\n    main()\n");
                util::write_file_raw(base + "/requirements.txt", "");
            } else if (type == "node") {
                util::write_file_raw(base + "/package.json",
                    "{\n  \"name\": \"" + name + "\",\n  \"version\": \"0.1.0\",\n  \"main\": \"index.js\"\n}\n");
                util::write_file_raw(base + "/index.js", "console.log('hello');\n");
            } else {
                mkdir((base + "/src").c_str(), 0755);
                util::write_file_raw(base + "/src/main.cpp",
                    "#include <iostream>\nint main(){ std::cout << \"hello\\n\"; }\n");
                util::write_file_raw(base + "/CMakeLists.txt",
                    "cmake_minimum_required(VERSION 3.16)\nproject(" + name + ")\n"
                    "add_executable(" + name + " src/main.cpp)\n");
            }
            util::write_file_raw(base + "/README.md", "# " + name + "\n");
            return "создан " + type + " project at " + base;
        }});

    add({"add_dependency", "Add a dependency using the detected package manager.", "code",
         schema({{"package","string","Package name",true}}), true, true,
        [](const json& a, ToolContext&) -> std::string {
            std::string pkg = sarg(a, "package"), cmd;
            if (util::file_exists("package.json")) cmd = "npm install " + util::shell_escape(pkg);
            else if (util::file_exists("Cargo.toml")) cmd = "cargo add " + util::shell_escape(pkg);
            else if (util::file_exists("requirements.txt") || has_cmd("pip")) cmd = "pip install " + util::shell_escape(pkg);
            else return "ошибка: не обнаружен менеджер пакетов";
            return clip(sh(cmd).second);
        }});

    add({"remove_dependency", "Remove a dependency.", "code",
         schema({{"package","string","Package name",true}}), true, true,
        [](const json& a, ToolContext&) -> std::string {
            std::string pkg = sarg(a, "package"), cmd;
            if (util::file_exists("package.json")) cmd = "npm uninstall " + util::shell_escape(pkg);
            else if (util::file_exists("Cargo.toml")) cmd = "cargo remove " + util::shell_escape(pkg);
            else cmd = "pip uninstall -y " + util::shell_escape(pkg);
            return clip(sh(cmd).second);
        }});

    add({"list_dependencies", "List project dependencies.", "code", schema({}), true, false,
        [](const json&, ToolContext&) -> std::string {
            if (util::file_exists("package.json")) { bool ok; return clip(util::read_file_raw("package.json", ok)); }
            if (util::file_exists("requirements.txt")) { bool ok; return clip(util::read_file_raw("requirements.txt", ok)); }
            if (util::file_exists("Cargo.toml")) { bool ok; return clip(util::read_file_raw("Cargo.toml", ok)); }
            return "манифест зависимостей не найден";
        }});

    add({"check_syntax", "Check the syntax of a source file.", "code",
         schema({{"path","string","File path",true}}), true, false,
        [](const json& a, ToolContext&) -> std::string {
            std::string p = util::expand_path(sarg(a, "path")), cmd;
            if (util::ends_with(p, ".py")) cmd = "python3 -m py_compile " + util::shell_escape(p);
            else if (util::ends_with(p, ".js")) cmd = "node --check " + util::shell_escape(p);
            else if (util::ends_with(p, ".cpp") || util::ends_with(p, ".cc") || util::ends_with(p, ".c"))
                cmd = "g++ -fsyntax-only -std=c++17 " + util::shell_escape(p);
            else return "ошибка: неподдерживаемый тип файла";
            auto r = sh(cmd);
            return r.first == 0 ? "синтаксис в порядке" : clip("ошибки синтаксиса:\n" + r.second);
        }});

    add({"format_code", "Format a source file with the appropriate formatter.", "code",
         schema({{"path","string","File path",true}}), true, true,
        [](const json& a, ToolContext&) -> std::string {
            std::string p = util::expand_path(sarg(a, "path")), cmd;
            if (util::ends_with(p, ".py") && has_cmd("black")) cmd = "black " + util::shell_escape(p);
            else if ((util::ends_with(p, ".cpp") || util::ends_with(p, ".h") || util::ends_with(p, ".hpp")) && has_cmd("clang-format"))
                cmd = "clang-format -i " + util::shell_escape(p);
            else if (has_cmd("prettier")) cmd = "prettier --write " + util::shell_escape(p);
            else return "ошибка: нет доступного форматтера";
            auto r = sh(cmd);
            return r.first == 0 ? "отформатирован " + sarg(a, "path") : clip(r.second);
        }});

    add({"analyze_complexity", "Report basic size/complexity metrics for a file.", "code",
         schema({{"path","string","File path",true}}), true, false,
        [](const json& a, ToolContext&) -> std::string {
            bool ok = false;
            std::string data = util::read_file_raw(sarg(a, "path"), ok);
            if (!ok) return "ошибка: не удаётся прочитать файл";
            auto lines = util::split(data, '\n');
            long blank = 0, branches = 0;
            for (auto& l : lines) {
                std::string t = util::trim(l);
                if (t.empty()) ++blank;
                if (t.find("if") != std::string::npos || t.find("for") != std::string::npos ||
                    t.find("while") != std::string::npos || t.find("case") != std::string::npos) ++branches;
            }
            return "lines=" + std::to_string(lines.size()) +
                   " code=" + std::to_string(lines.size() - blank) +
                   " blank=" + std::to_string(blank) +
                   " branch_points=" + std::to_string(branches);
        }});

    // мета-тулзы (fix/explain/refactor/...) — это хитрожопый трюк: реально код правит
    // сама модель, а тулза просто читает файл и отдаёт его обратно с пометкой «вот задача».
    // exposed=false, поэтому в схему для API они не попадают, но в /tools их видно и они считаются
    auto code_helper = [](const std::string& verb) {
        return [verb](const json& a, ToolContext&) -> std::string {
            bool ok = false;
            std::string data = util::read_file_raw(sarg(a, "path"), ok);
            if (!ok) return "ошибка: не удаётся прочитать " + sarg(a, "path");
            return "[задача: " + verb + "]\n" + clip(data);
        };
    };
    for (auto& v : {std::pair<std::string,std::string>{"fix_code","Locate and fix bugs in a file"},
                    {"explain_code","Explain what a file does"},
                    {"refactor_code","Refactor a file for clarity"},
                    {"generate_tests","Generate tests for a file"},
                    {"generate_docs","Generate documentation for a file"},
                    {"optimize_code","Optimize a file for performance"},
                    {"generate_diagram","Describe a diagram of a file's structure"}}) {
        add({v.first, v.second + ".", "code",
             schema({{"path","string","File path",true}}), false, false, code_helper(v.second)});
    }

    // ---------- 11. Planning ----------
    add({"schedule_task", "Add a task to the task list.", "planning",
         schema({{"description","string","Task description",true}}), true, false,
        [](const json& a, ToolContext& ctx) -> std::string {
            return "задача #" + std::to_string(ctx.history.add_task(sarg(a, "description"))) + " запланирована";
        }});

    add({"list_tasks", "List all tasks.", "planning", schema({}), true, false,
        [](const json&, ToolContext& ctx) -> std::string {
            auto ts = ctx.history.list_tasks();
            if (ts.empty()) return "задач нет";
            std::string out;
            for (auto& t : ts) out += "#" + std::to_string(t.id) + " [" + t.status + "] " + t.description + "\n";
            return out;
        }});

    add({"cancel_task", "Cancel a task by id.", "planning",
         schema({{"id","integer","Task id",true}}), true, false,
        [](const json& a, ToolContext& ctx) -> std::string {
            return ctx.history.set_task_status(iarg(a, "id"), "отменена") ? "отменена" : "ошибка: такой задачи нет";
        }});

    add({"run_task", "Mark a task as running.", "planning",
         schema({{"id","integer","Task id",true}}), true, false,
        [](const json& a, ToolContext& ctx) -> std::string {
            return ctx.history.set_task_status(iarg(a, "id"), "running") ? "running" : "ошибка: такой задачи нет";
        }});

    add({"task_status", "Show the status of a task.", "planning",
         schema({{"id","integer","Task id",true}}), true, false,
        [](const json& a, ToolContext& ctx) -> std::string {
            for (auto& t : ctx.history.list_tasks())
                if (t.id == iarg(a, "id")) return "#" + std::to_string(t.id) + " " + t.status;
            return "ошибка: такой задачи нет";
        }});

    // ---------- 12. Security ----------
    add({"scan_security", "Scan a path for hardcoded secrets and risky calls.", "security",
         schema({{"path","string","Root (default .)",false}}), true, false,
        [](const json& a, ToolContext&) -> std::string {
            std::string p = util::expand_path(sarg(a, "path", "."));
            std::string pat = "(api[_-]?key|secret|password|token|BEGIN RSA|eval\\(|system\\(|exec\\()";
            auto r = sh("grep -rnIE " + util::shell_escape(pat) + " " + util::shell_escape(p) +
                        " 2>/dev/null | head -50");
            return r.second.empty() ? "явных проблем не найдено" : clip("возможные находки:\n" + r.second);
        }});

    add({"check_permissions", "Show file permissions and ownership.", "security",
         schema({{"path","string","File path",true}}), true, false,
        [](const json& a, ToolContext&) -> std::string {
            return clip(sh("ls -ld " + util::shell_escape(util::expand_path(sarg(a, "path"))) +
                           "; stat -c '%A %U:%G %a' " + util::shell_escape(util::expand_path(sarg(a, "path")))).second);
        }});

    add({"encrypt_file", "Encrypt a file with AES-256 (openssl).", "security",
         schema({{"path","string","File path",true},{"password","string","Password",true}}), true, true,
        [](const json& a, ToolContext&) -> std::string {
            if (!has_cmd("openssl")) return "ошибка: openssl не установлен";
            std::string p = util::expand_path(sarg(a, "path"));
            // pass:<пароль> в командной строке — пароль на секунду виден в `ps aux`. некрасиво,
            // но shell_escape хотя бы спасает от инъекции. для пароля по-хорошему нужен stdin/файл
            auto r = sh("openssl enc -aes-256-cbc -pbkdf2 -salt -in " + util::shell_escape(p) +
                        " -out " + util::shell_escape(p + ".enc") + " -pass pass:" +
                        util::shell_escape(sarg(a, "password")));
            return r.first == 0 ? "зашифровано в " + sarg(a, "path") + ".enc" : "ошибка: " + r.second;
        }});

    add({"decrypt_file", "Decrypt an AES-256 file (openssl).", "security",
         schema({{"path","string","Encrypted .enc file",true},{"password","string","Password",true}}), true, true,
        [](const json& a, ToolContext&) -> std::string {
            if (!has_cmd("openssl")) return "ошибка: openssl не установлен";
            std::string p = util::expand_path(sarg(a, "path"));
            std::string out = util::ends_with(p, ".enc") ? p.substr(0, p.size() - 4) : p + ".dec";
            auto r = sh("openssl enc -d -aes-256-cbc -pbkdf2 -in " + util::shell_escape(p) +
                        " -out " + util::shell_escape(out) + " -pass pass:" +
                        util::shell_escape(sarg(a, "password")));
            return r.first == 0 ? "расшифровано в " + out : "ошибка: " + r.second;
        }});

    add({"hash_file", "Compute a hash of a file.", "security",
         schema({{"path","string","File path",true},{"algo","string","md5|sha1|sha256 (default sha256)",false}}),
         true, false,
        [](const json& a, ToolContext&) -> std::string {
            std::string algo = util::to_lower(sarg(a, "algo", "sha256"));
            std::string tool = algo == "md5" ? "md5sum" : algo == "sha1" ? "sha1sum" : "sha256sum";
            return clip(sh(tool + " " + util::shell_escape(util::expand_path(sarg(a, "path")))).second);
        }});

    // ---------- 13. Utilities ----------
    add({"calc", "Evaluate an arithmetic expression.", "util",
         schema({{"expression","string","e.g. (2+3)*4^2",true}}), true, false,
        [](const json& a, ToolContext&) -> std::string {
            std::string e = sarg(a, "expression");
            Calc c(e);
            double v = c.parse();
            if (c.err) return "ошибка: некорректное выражение";
            std::ostringstream ss; ss << v;
            return ss.str();
        }});

    add({"random", "Generate a random integer in [min, max].", "util",
         schema({{"min","integer","Lower bound (default 0)",false},{"max","integer","Upper bound (default 100)",false}}),
         true, false,
        [](const json& a, ToolContext&) -> std::string {
            long lo = iarg(a, "min", 0), hi = iarg(a, "max", 100);
            if (hi < lo) std::swap(lo, hi);
            static std::mt19937_64 rng(std::random_device{}());
            std::uniform_int_distribution<long> d(lo, hi);
            return std::to_string(d(rng));
        }});

    add({"time", "Current local time.", "util", schema({}), true, false,
        [](const json&, ToolContext&) -> std::string {
            std::time_t t = std::time(nullptr); char b[32];
            std::strftime(b, sizeof b, "%H:%M:%S", std::localtime(&t)); return b;
        }});

    add({"date", "Current local date.", "util", schema({}), true, false,
        [](const json&, ToolContext&) -> std::string {
            std::time_t t = std::time(nullptr); char b[32];
            std::strftime(b, sizeof b, "%Y-%m-%d (%A)", std::localtime(&t)); return b;
        }});

    add({"weather", "Get the weather for a city (wttr.in).", "util",
         schema({{"city","string","City name",true}}), true, false,
        [](const json& a, ToolContext&) -> std::string {
            std::string city = sarg(a, "city");
            std::string url = "https://wttr.in/";
            for (char c : city) url += (c == ' ') ? '+' : c;
            url += "?format=3";
            auto r = http::get(url);
            return r.ok() ? util::trim(r.body) : "ошибка: погода недоступна";
        }});
}

}
