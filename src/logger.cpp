#include "logger.hpp"
#include "util.hpp"

namespace nexus {

static const char* level_name(LogLevel l) {
    switch (l) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "?";
}

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

void Logger::init(const std::string& path, LogLevel min_level) {
    std::lock_guard<std::mutex> lock(mu_);
    min_level_ = min_level;
    file_.open(util::expand_path(path), std::ios::app);
}

void Logger::log(LogLevel level, const std::string& msg) {
    if (level < min_level_) return;          // ниже порога — даже мьютекс не трогаем
    std::lock_guard<std::mutex> lock(mu_);
    if (!file_.is_open()) return;            // лог не открылся (нет прав на папку?) — молча игнорим, не ронять же агента из-за лога
    file_ << util::now_iso() << " [" << level_name(level) << "] " << msg << "\n";
    file_.flush();                           // флашим каждую строку: если агент крашнется, лог должен уцелеть
}

}
