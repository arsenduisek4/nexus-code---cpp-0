#pragma once

#include "common.hpp"
#include "config.hpp"

namespace nexus {

struct ChatResult {
    bool ok = false;
    std::string error;
    json message;
    int prompt_tokens = 0;
    int completion_tokens = 0;
};

class NexusClient {
public:
    explicit NexusClient(const Config& cfg) : cfg_(cfg) {}

    ChatResult chat(const json& messages, const json& tools);
    bool ping();

private:
    const Config& cfg_;
};

}
