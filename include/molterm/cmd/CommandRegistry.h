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
};

class CommandRegistry {
public:
    void registerCmd(const std::string& name, CommandHandler handler,
                     const std::string& usage = "",
                     const std::string& description = "");

    // Execute a command string, returns structured result
    ExecResult execute(Application& app, const std::string& input);

    // Tab completion
    std::vector<std::string> complete(const std::string& prefix) const;

    bool hasCommand(const std::string& name) const;

private:
    std::unordered_map<std::string, CommandInfo> commands_;
};

} // namespace molterm
