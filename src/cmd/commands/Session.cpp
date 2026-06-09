// Session-lifecycle commands: :q / :quit / :qa.

#include "molterm/cmd/commands/Commands.h"

#include "molterm/app/Application.h"
#include "molterm/cmd/CommandParser.h"
#include "molterm/cmd/CommandRegistry.h"

namespace molterm {

void registerSessionCommands(Application& /*app*/, CommandRegistry& reg) {
    // :q / :quit
    reg.registerCmd("q", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        app.quit(cmd.forced);
        return {true, ""};
    }, ":q[!]", "Quit MolTerm (use :q! to skip auto-save)",
       {":q", ":q!"}, "Help");
    reg.registerCmd("quit", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        app.quit(cmd.forced);
        return {true, ""};
    }, ":quit[!]", "Quit MolTerm (alias for :q)",
       {":quit"}, "Help");
    reg.registerCmd("qa", [](Application& app, const ParsedCommand&) -> ExecResult {
        app.quit(true);
        return {true, ""};
    }, ":qa", "Quit all tabs and exit",
       {":qa"}, "Help");
}

}  // namespace molterm
