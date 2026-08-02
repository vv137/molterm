#include "molterm/cmd/CommandRegistry.h"

#include "molterm/app/Application.h"
#include "molterm/app/CommandScope.h"
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

    // Vim-style bang ":foo!" flips command scope for this single
    // dispatch (scope=all → current, current → all). Commands that
    // explicitly call forEachInScope(app, ScopeMode::Current, ...) are
    // unaffected (e.g. :delete pins to current regardless), and :q
    // continues to read cmd.forced for its own force-quit semantics —
    // setting a scope override on top is a no-op for those handlers.
    const bool flipScope = cmd.forced;
    if (flipScope) {
        ScopeMode flipped = (app.commandScope() == ScopeMode::All)
                                ? ScopeMode::Current : ScopeMode::All;
        app.setScopeOverride(flipped);
    }
    // Backstop: a handler that throws (e.g. an unguarded std::stoi on
    // malformed user/script input) must surface as a failed command, not
    // crash the whole TUI session or abort a batch script. Convert any
    // std::exception into an error result. Individual handlers still do
    // their own validation for friendlier per-option messages; this is
    // the safety net that keeps one bad argument from being fatal.
    // Selection-parse errors are scoped to this command's own expressions.
    // Dispatch nests — `:run` and the control-flow commands execute their body
    // through this same path — so take the slot on the way in and give it back
    // on the way out. Clearing on exit alone is what keeps a child's bad
    // selection from replacing `:run`'s "N commands, 1 failed" summary; saving
    // the caller's covers the reverse, a handler that parses a selection and
    // then nests a dispatch. No handler does both today.
    std::string outerErr = app.takeSelectionError();

    ExecResult r;
    try {
        r = it->second.handler(app, cmd);
    } catch (const std::exception& e) {
        r = {false, std::string("error: ") + e.what()};
    }
    if (flipScope) app.clearScopeOverride();

    std::string ownErr = app.takeSelectionError();
    app.noteSelectionError(outerErr);

    // A selection the grammar could not fully consume fails the command, even
    // if the handler was happy with the prefix that did parse. That prefix is
    // the whole danger: `:color red chain A resi 5` used to colour chain A and
    // report success, so a wrong figure looked like a good one (issue #121).
    // Enforced here so it holds for every command, including future ones.
    if (!ownErr.empty()) r = {false, "Bad selection: " + ownErr};
    return r;
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
