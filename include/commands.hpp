#pragma once

#include <string>

namespace nexus {

class Agent;

struct CommandOutcome {
    bool handled = false;
    bool quit = false;
};

CommandOutcome handle_command(Agent& agent, const std::string& input);

}
