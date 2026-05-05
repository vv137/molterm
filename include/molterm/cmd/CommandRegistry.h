#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace molterm {

class Application;
struct ParsedCommand;

struct ExecResult {
    bool ok;
    std::string msg;
};

using CommandHandler = std::function<ExecResult(Application&, const ParsedCommand&)>;

struct CommandInfo {
    std::string name;
    CommandHandler handler;
    std::string usage;
    std::string description;
    std::vector<std::string> examples;
    std::string category;
};

class CommandRegistry {
public:
    void registerCmd(const std::string& name, CommandHandler handler,
                     const std::string& usage = "",
                     const std::string& description = "",
                     std::vector<std::string> examples = {},
                     const std::string& category = "");

    // Execute a command string, returns structured result
    ExecResult execute(Application& app, const std::string& input);

    // Tab completion
    std::vector<std::string> complete(const std::string& prefix) const;

    bool hasCommand(const std::string& name) const;

    const CommandInfo* lookup(const std::string& name) const;

    // Iterate all registered commands (for :help index)
    const std::unordered_map<std::string, CommandInfo>& all() const { return commands_; }

private:
    std::unordered_map<std::string, CommandInfo> commands_;
};

} // namespace molterm
