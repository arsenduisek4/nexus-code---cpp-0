#pragma once

#include "common.hpp"
#include "config.hpp"
#include "api.hpp"
#include "tools.hpp"
#include "history.hpp"
#include "memory.hpp"

namespace nexus {

class Agent {
public:
    Agent(Config& cfg, History& history, Memory& memory);
    void run_turn(const std::string& user_input);

    Config& config() { return cfg_; }
    History& history() { return history_; }
    Memory& memory() { return memory_; }
    ToolRegistry& tools() { return tools_; }
    bool& auto_approve() { return cfg_.auto_approve; }

private:
    json build_messages(const std::string& user_input);
    std::string system_prompt();
    bool confirm(const std::string& summary);

    Config& cfg_;
    History& history_;
    Memory& memory_;
    NexusClient client_;
    ToolRegistry tools_;
};

}
