#pragma once

#include "common.hpp"

namespace nexus {

class Config;
class History;
class Memory;

struct ToolContext {
    Config& cfg;
    History& history;
    Memory& memory;
    bool& auto_approve;
};

struct Tool {
    std::string name;
    std::string description;
    std::string category;
    json parameters;
    bool exposed = true;
    bool mutating = false;
    std::function<std::string(const json&, ToolContext&)> run;
};

class ToolRegistry {
public:
    void register_all();

    void add(Tool t);
    const Tool* find(const std::string& name) const;
    json schemas() const;
    std::vector<std::string> names() const;
    std::map<std::string, std::vector<std::string>> by_category() const;
    size_t size() const { return tools_.size(); }

    std::string call(const std::string& name, const json& args, ToolContext& ctx) const;

private:
    std::map<std::string, Tool> tools_;
};

}
