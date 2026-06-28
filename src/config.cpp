#include "config.hpp"
#include "util.hpp"

#include <cstdlib>
#include <sys/stat.h>

namespace nexus {

static void ensure_dir(const std::string& path) {
    // mkdir вернёт -1 если папка уже есть — и похуй, мы ошибку не проверяем специально.
    // нам важно чтоб она просто БЫЛА, а не кто её создал
    mkdir(util::expand_path(path).c_str(), 0755);
}

std::string Config::config_path() const { return data_dir + "/config.json"; }
std::string Config::db_path() const     { return data_dir + "/nexus_cpp.db"; }
std::string Config::log_path() const    { return data_dir + "/logs/agent.log"; }

Config Config::load() {
    Config c;
    ensure_dir(c.data_dir);
    ensure_dir(c.data_dir + "/logs");

    bool ok = false;
    std::string text = util::read_file_raw(c.config_path(), ok);
    if (ok && !text.empty()) {
        try {
            json j = json::parse(text);
            c.raw = j;
            // тупо тянем поля по одному. да, можно было через рефлексию/макросы,
            // но в C++ рефлексии нет, а макросы — это боль, так что вот так, в лоб
            if (j.contains("api_key"))        c.api_key = j["api_key"].get<std::string>();
            if (j.contains("base_url"))       c.base_url = j["base_url"].get<std::string>();
            if (j.contains("model"))          c.model = j["model"].get<std::string>();
            if (j.contains("mode"))           c.mode = j["mode"].get<std::string>();
            if (j.contains("temperature"))    c.temperature = j["temperature"].get<double>();
            if (j.contains("max_iterations")) c.max_iterations = j["max_iterations"].get<int>();
            if (j.contains("auto_approve"))   c.auto_approve = j["auto_approve"].get<bool>();
            if (j.contains("log_level"))      c.log_level = j["log_level"].get<std::string>();
        } catch (...) {
            // конфиг кривой? хуй с ним, едем на дефолтах, не ронять же агента из-за
            // одной потерянной запятой в json
        }
    }

    // env перебивает файл — удобно для CI и чтоб ключ в конфиге не светить
    if (const char* env = std::getenv("NEXUS_API_KEY"))
        if (*env) c.api_key = env;

    return c;
}

void Config::save() const {
    // стартуем с raw, чтоб НЕ проебать чужие поля, которые юзер дописал руками
    json j = raw;
    j["base_url"]       = base_url;
    j["model"]          = model;
    j["mode"]           = mode;
    j["temperature"]    = temperature;
    j["max_iterations"] = max_iterations;
    j["auto_approve"]   = auto_approve;
    j["log_level"]      = log_level;
    if (!api_key.empty()) j["api_key"] = api_key;  // пустой ключ не пишем, нехуй мусорить
    util::write_file_raw(config_path(), j.dump(2));
}

}
