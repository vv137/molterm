// File / tab / object-listing commands: :load, :e, :tabnew, :tabclose, :objects, :object.


#include "molterm/app/Application.h"
#include "molterm/core/StringParse.h"
#include "molterm/cmd/CommandParser.h"
#include "molterm/cmd/CommandRegistry.h"
#include "molterm/app/PathPatterns.h"

namespace molterm {

void Application::registerFilesCommands(CommandRegistry& reg) {
    // :load <file>
    auto loadPatterns = [](Application& app, const ParsedCommand& cmd,
                           const char* usage) -> ExecResult {
        if (cmd.args.empty()) return {false, usage};
        // Fast path for the common single-literal case: skip globbing
        // entirely so a path with no metacharacters and no brace range
        // never has to round-trip through glob() / readdir().
        const bool singleLiteral =
            cmd.args.size() == 1 &&
            cmd.args[0].find_first_of("*?[{") == std::string::npos;
        std::vector<std::string> matches =
            singleLiteral ? std::vector<std::string>{cmd.args[0]}
                          : expandPathPatterns(cmd.args);
        if (matches.empty()) {
            std::string joined;
            for (size_t i = 0; i < cmd.args.size(); ++i) {
                if (i) joined += ' ';
                joined += cmd.args[i];
            }
            return {false, "No files matched pattern(s): " + joined};
        }
        if (matches.size() == 1) {
            std::string msg = app.loadFile(matches[0]);
            bool ok = msg.rfind("Loaded ", 0) == 0;
            return {ok, msg};
        }
        int loaded = 0;
        std::string firstError;
        for (const auto& p : matches) {
            std::string msg = app.loadFile(p);
            if (msg.rfind("Loaded ", 0) == 0) ++loaded;
            else if (firstError.empty()) firstError = msg;
        }
        if (loaded == 0)
            return {false, "All loads failed; first error: " + firstError};
        std::string summary = "Loaded " + std::to_string(loaded) +
                              " structure(s)";
        const int failed = static_cast<int>(matches.size()) - loaded;
        if (failed > 0) summary += " (" + std::to_string(failed) + " failed)";
        return {true, summary};
    };

    reg.registerCmd("load",
        [loadPatterns](Application& app, const ParsedCommand& cmd) -> ExecResult {
            return loadPatterns(app, cmd, "Usage: :load <pattern>...");
        },
        ":load <pattern>...",
        "Load structure file(s); supports shell globs and brace ranges",
        {":load protein.pdb", ":load 1bna.cif",
         ":load *.pdb", ":load model_{1..5}.cif",
         ":load relaxed_*.pdb confident_*.cif"},
        "Files");
    reg.registerCmd("e",
        [loadPatterns](Application& app, const ParsedCommand& cmd) -> ExecResult {
            return loadPatterns(app, cmd, "Usage: :e <pattern>...");
        },
        ":e <pattern>...",
        "Load structure file(s) (alias for :load)",
        {":e protein.pdb", ":e *.cif"},
        "Files");

    // :tabnew
    reg.registerCmd("tabnew", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        std::string name = cmd.args.empty() ? "" : cmd.args[0];
        app.tabs().addTab(name);
        app.tabs().goToTab(static_cast<int>(app.tabs().count()) - 1);
        return {true, "New tab created"};
    }, ":tabnew [name]", "Create a new tab (optionally named)",
       {":tabnew", ":tabnew analysis"}, "Window");

    // :tabclose
    reg.registerCmd("tabclose", [](Application& app, const ParsedCommand&) -> ExecResult {
        if (app.tabs().count() <= 1) return {false, "Cannot close last tab"};
        app.tabs().closeCurrentTab();
        return {true, ""};
    }, ":tabclose", "Close the current tab",
       {":tabclose"}, "Window");

    // :objects
    reg.registerCmd("objects", [](Application& app, const ParsedCommand&) -> ExecResult {
        auto names = app.store().names();
        if (names.empty()) return {true, "No objects loaded"};
        std::string result = "Objects:";
        for (const auto& n : names) result += " " + n;
        return {true, result};
    }, ":objects", "List objects loaded in the current tab",
       {":objects"}, "Window");

    // :object — make a specific object the "current" one (drives the info
    // panel, structure-mutating commands, and per-tab seq bar). It does
    // NOT gate :color/:show/:hide; those follow :set scope.
    reg.registerCmd("object", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        auto& tab = app.tabs().currentTab();
        const auto& objs = tab.objects();
        if (cmd.args.empty()) {
            auto cur = tab.currentObject();
            if (!cur) return {true, "No current object (tab is empty)"};
            return {true, "Current object: " + cur->name() + " [" +
                          std::to_string(tab.selectedObjectIdx() + 1) + "/" +
                          std::to_string(objs.size()) + "]"};
        }
        if (objs.empty()) return {false, "No objects loaded"};

        const auto& a = cmd.args[0];
        if (a == "next") {
            tab.selectNextObject();
        } else if (a == "prev" || a == "previous") {
            tab.selectPrevObject();
        } else {
            // Try as 1-based index first (matches the listing in :objects),
            // fall back to name lookup.
            int newIdx = -1;
            if (auto n = parseInt(a); n && *n >= 1 &&
                    *n <= static_cast<int>(objs.size())) {
                newIdx = *n - 1;
            }
            if (newIdx < 0) {
                for (size_t i = 0; i < objs.size(); ++i) {
                    if (objs[i] && objs[i]->name() == a) {
                        newIdx = static_cast<int>(i);
                        break;
                    }
                }
            }
            if (newIdx < 0) return {false, "No such object: " + a};
            tab.selectObject(newIdx);
        }

        // Per-object overlays (interface mask + contacts) reference the
        // prior object's atom indices; refresh against the new mol so the
        // overlay matches what's actually being rendered.
        app.onCurrentObjectChanged();
        auto cur = tab.currentObject();
        // Refresh the panel + seqbar so the UI follows the switch.
        app.layout().markDirty(Layout::Component::ObjectPanel);
        app.layout().markDirty(Layout::Component::SeqBar);
        if (!cur) return {true, "No current object"};
        return {true, "Current object: " + cur->name() + " [" +
                      std::to_string(tab.selectedObjectIdx() + 1) + "/" +
                      std::to_string(objs.size()) + "]"};
    }, ":object [name|index|next|prev]",
       "Show or set the current object in the active tab",
       {":object", ":object 1ubq", ":object 2", ":object next"},
       "Window");
}

}  // namespace molterm
