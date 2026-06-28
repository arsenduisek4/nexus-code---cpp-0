#pragma once

#include <string>
#include <fstream>
#include <mutex>

namespace nexus {

enum class LogLevel { Debug, Info, Warn, Error };

class Logger {
public:
    static Logger& instance();
    void init(const std::string& path, LogLevel min_level);
    void log(LogLevel level, const std::string& msg);

    void debug(const std::string& m) { log(LogLevel::Debug, m); }
    void info(const std::string& m)  { log(LogLevel::Info, m); }
    void warn(const std::string& m)  { log(LogLevel::Warn, m); }
    void error(const std::string& m) { log(LogLevel::Error, m); }

private:
    std::ofstream file_;
    LogLevel min_level_ = LogLevel::Info;
    std::mutex mu_;
};

}
