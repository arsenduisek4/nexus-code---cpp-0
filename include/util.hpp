#pragma once

#include <string>
#include <vector>

namespace nexus::util {

std::string home_dir();
std::string expand_path(const std::string& path);
std::string read_file_raw(const std::string& path, bool& ok);
bool write_file_raw(const std::string& path, const std::string& data);
bool file_exists(const std::string& path);
std::string trim(const std::string& s);
std::vector<std::string> split(const std::string& s, char delim);
std::string join(const std::vector<std::string>& parts, const std::string& sep);
std::string now_iso();
std::string shell_escape(const std::string& arg);
std::string to_lower(const std::string& s);
bool ends_with(const std::string& s, const std::string& suffix);
bool starts_with(const std::string& s, const std::string& prefix);

}
