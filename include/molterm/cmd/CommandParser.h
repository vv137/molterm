#pragma once

#include <string>
#include <vector>

namespace molterm {

struct ParsedCommand {
    std::string name;
    std::vector<std::string> args;
    bool forced = false;  // e.g., :q! sets forced=true
};

class CommandParser {
public:
    static ParsedCommand parse(const std::string& input);
};

} // namespace molterm
