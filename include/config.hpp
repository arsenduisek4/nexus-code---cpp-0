#pragma once

#include "common.hpp"

namespace nexus {

struct Config {
    std::string api_key;
    std::string base_url   = "https://api.nexuscode.ai";
    std::string model      = "nexus-code";
    std::string mode       = "agent";
    double      temperature = 0.3;
    int         max_iterations = 0;
    bool        auto_approve   = false;
    std::string data_dir   = "~/.nexuscode";
    std::string log_level  = "info";

    json raw = json::object();

    static Config load();
    void save() const;
    std::string config_path() const;
    std::string db_path() const;
    std::string log_path() const;
};

}
