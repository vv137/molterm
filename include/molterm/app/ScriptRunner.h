#pragma once

#include <istream>
#include <string>
#include <unordered_map>
#include <vector>

#include "molterm/app/Application.h"

namespace molterm {

// The molterm script execution engine (issues #67 / #68 / #80). Owns the
// block-aware dispatcher (`:if/:elseif/:else/:endif`, `:foreach`, `:def`
// user functions), the user-function table, and recursion guarding. Reaches
// script *context* — registers, env, frames, the command registry — through
// the Application it runs against. Split out of Application so the ~600-line
// engine no longer lives on the app object.
class ScriptRunner {
public:
    explicit ScriptRunner(Application& app) : app_(app) {}

    // Run commands from a stream: peel the optional `#!molterm` shebang, set
    // up a local register/env frame when the script declares scope=local or
    // is invoked with args, buffer the body, then dispatch it. This is the
    // former Application::runScriptStream body.
    Application::ScriptRunResult run(
        std::istream& in, bool strict,
        const std::unordered_map<std::string, std::string>& args,
        const std::string& sourcePath);

    // Run a single interactively-entered line through the script dispatcher
    // so it gets identical handling to scripts: ';'-separated commands,
    // ${var} expansion, '#' comments, and user-defined :def functions. The
    // TUI command-entry path. `result` carries the first failure / last
    // message back for the status line + transcript.
    void runLine(const std::string& line, Application::ScriptRunResult& result);

private:
    // Control-flow outcome of dispatching a (sub)range of script lines.
    // Normal: ran to the end. Stopped: strict-mode error halt. Break /
    // Continue: a :break / :continue propagating up to the enclosing
    // :foreach. Return: a :return unwinding to the script-frame boundary.
    enum class ScriptFlow { Normal, Stopped, Break, Continue, Return };

    // Block-aware dispatcher: walks lines [lo, hi) tracking control flow,
    // re-entering itself for :foreach bodies and :def function calls.
    ScriptFlow dispatch(const std::vector<std::string>& lines,
                        size_t lo, size_t hi,
                        Application::ScriptRunResult& result, bool strict);

    // User-defined script functions (`:def name(p1, ...) ... :enddef`).
    // Calling `name a b` runs the body in a fresh local frame with the
    // params seeded as env vars; scriptFnDepth_ guards runaway recursion.
    struct UserScriptFn { std::vector<std::string> params; std::vector<std::string> body; };

    // The register/env *frame stack* (regStack_/envStack_, pushScriptFrame,
    // ScriptFrameGuard, markExport) intentionally stays on Application — it's
    // shared with the :let / :export / :setenv command handlers — so this
    // engine drives the frame lifecycle through app_ rather than owning it.
    Application& app_;
    std::unordered_map<std::string, UserScriptFn> userScriptFns_;
    int scriptFnDepth_ = 0;
};

} // namespace molterm
