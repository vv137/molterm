#include "molterm/cmd/CommandRegistry.h"
#include "molterm/cmd/CommandParser.h"

#include <algorithm>

namespace molterm {

void CommandRegistry::registerCmd(const std::string& name, CommandHandler handler,
                                   const std::string& usage,
                                   const std::string& description,
                                   std::vector<std::string> examples,
                                   const std::string& category) {
    commands_[name] = {name, std::move(handler), usage, description,
                       std::move(examples), category};
}

ExecResult CommandRegistry::execute(Application& app, const std::string& input) {
    auto cmd = CommandParser::parse(input);
    if (cmd.name.empty()) return {true, ""};

    auto it = commands_.find(cmd.name);
    if (it == commands_.end()) {
        return {false, "Unknown command: " + cmd.name};
    }

    return it->second.handler(app, cmd);
}

std::vector<std::string> CommandRegistry::complete(const std::string& prefix) const {
    std::vector<std::string> results;
    for (const auto& [name, info] : commands_) {
        if (name.find(prefix) == 0) {
            results.push_back(name);
        }
    }
    std::sort(results.begin(), results.end());
    return results;
}

bool CommandRegistry::hasCommand(const std::string& name) const {
    return commands_.count(name) > 0;
}

const CommandInfo* CommandRegistry::lookup(const std::string& name) const {
    auto it = commands_.find(name);
    if (it == commands_.end()) return nullptr;
    return &it->second;
}

} // namespace molterm
