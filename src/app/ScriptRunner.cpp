#include "molterm/app/ScriptRunner.h"

#include "molterm/cmd/CommandHelpers.h"
#include "molterm/cmd/CommandParser.h"
#include "molterm/cmd/CommandRegistry.h"
#include "molterm/cmd/ExprContext.h"
#include "molterm/cmd/RegisterExpr.h"
#include "molterm/core/Logger.h"
#include "molterm/core/MolObject.h"
#include "molterm/core/Selection.h"
#include "molterm/tui/Screen.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace molterm {

namespace {
// Strip a trailing '#' comment, respecting single/double quotes so a '#'
// inside a quoted argument (e.g. a label string) is preserved. `#` only
// starts a comment when it's at the line start or preceded by whitespace
// AND followed by whitespace or end-of-line — so mid-token hashes like
// `:color #cfcfcf` (hex literal, issue #92) survive intact. Inline
// trailing comments (`:foo bar # note`) still strip as expected.
// Backslash escapes (`\#`) are NOT honoured.
void stripScriptComment(std::string& s) {
    bool inQuote = false;
    char qc = '\0';
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (inQuote) {
            if (c == qc) inQuote = false;
        } else if (c == '"' || c == '\'') {
            inQuote = true; qc = c;
        } else if (c == '#') {
            bool leftBoundary = (i == 0) ||
                std::isspace(static_cast<unsigned char>(s[i - 1]));
            bool rightBoundary = (i + 1 == s.size()) ||
                std::isspace(static_cast<unsigned char>(s[i + 1]));
            if (leftBoundary && rightBoundary) {
                s.resize(i);
                return;
            }
        }
    }
}

// True iff `line` (after leading whitespace + optional `:`) starts
// with the keyword `kw` followed by whitespace or end-of-line. On hit,
// fills `body` with the rest of the line (after the keyword).
bool isControlKeyword(const std::string& line, const char* kw, std::string& body) {
    size_t i = line.find_first_not_of(" \t");
    if (i == std::string::npos) return false;
    if (line[i] == ':') {
        ++i;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    }
    size_t klen = std::strlen(kw);
    if (line.compare(i, klen, kw) != 0) return false;
    size_t after = i + klen;
    if (after < line.size() && line[after] != ' ' && line[after] != '\t') return false;
    while (after < line.size() && (line[after] == ' ' || line[after] == '\t')) ++after;
    body = line.substr(after);
    // trim trailing whitespace
    size_t end = body.find_last_not_of(" \t\r");
    if (end == std::string::npos) body.clear();
    else                          body.resize(end + 1);
    return true;
}
}  // namespace

Application::ScriptRunResult ScriptRunner::run(
    std::istream& in, bool strict,
    const std::unordered_map<std::string, std::string>& args,
    const std::string& sourcePath) {
    Application::ScriptRunResult result;
    result.sourcePath = sourcePath.empty() ? std::string("<stdin>") : sourcePath;

    // Peek the first non-empty line for a `#!molterm` shebang. The
    // shebang is grammar-light:
    //   #!molterm scope=local export=name1,name2
    // Recognised keys: scope (local|inherit, default inherit), export
    // (comma list of names that flow back to the caller).
    enum class Scope { Inherit, Local };
    Scope scope = Scope::Inherit;
    std::vector<std::string> exports;
    std::string firstLine;
    bool consumedShebang = false;
    if (std::getline(in, firstLine)) {
        std::string t = firstLine;
        size_t s = t.find_first_not_of(" \t");
        if (s != std::string::npos &&
            t.compare(s, 9, "#!molterm") == 0) {
            consumedShebang = true;
            std::string rest = t.substr(s + 9);
            std::stringstream ss(rest);
            std::string tok;
            while (ss >> tok) {
                auto eq = tok.find('=');
                if (eq == std::string::npos) continue;
                std::string k = tok.substr(0, eq);
                std::string v = tok.substr(eq + 1);
                if (k == "scope") {
                    if      (v == "local")   scope = Scope::Local;
                    else if (v == "inherit") scope = Scope::Inherit;
                } else if (k == "export") {
                    std::string name;
                    for (char c : v) {
                        if (c == ',') { if (!name.empty()) exports.push_back(name); name.clear(); }
                        else if (!std::isspace(static_cast<unsigned char>(c))) name += c;
                    }
                    if (!name.empty()) exports.push_back(name);
                }
            }
        }
    }
    // Args alone imply scope=local even without a shebang — so `:run X
    // KEY=VAL` always isolates the script's writes from the caller.
    // RAII guard ensures every early-return / exception path through
    // the loop body still pops the frame exactly once.
    bool needLocal = (scope == Scope::Local) || !args.empty();
    Application::ScriptFrameGuard frame(app_, needLocal, args, exports);

    // Buffer the script body (post-shebang) into a line vector so the
    // block-aware dispatcher can find matching :endif / :end without
    // re-reading the stream. srcLineOffset translates buffer index back
    // to the original 1-indexed file line number for failure reports
    // (issue #80) — if we consumed a shebang on line 1, buffer[0] is
    // file line 2.
    std::vector<std::string> lines;
    if (!firstLine.empty() && !consumedShebang) lines.push_back(std::move(firstLine));
    result.srcLineOffset = consumedShebang ? 2 : 1;
    std::string line;
    while (std::getline(in, line)) lines.push_back(std::move(line));
    dispatch(lines, 0, lines.size(), result, strict);
    return result;                                  // RAII pops frame
}

void ScriptRunner::runLine(const std::string& line,
                           Application::ScriptRunResult& result) {
    std::vector<std::string> buf = {line};
    dispatch(buf, 0, 1, result, /*strict=*/false);
}

ScriptRunner::ScriptFlow ScriptRunner::dispatch(
        const std::vector<std::string>& lines,
        size_t lo, size_t hi,
        Application::ScriptRunResult& result, bool strict) {
    using Flow = ScriptFlow;
    // Centralise the "record a failure" path so every report (command
    // exec error, bad :if condition, malformed :foreach header, …)
    // contributes uniform context to result.failureList — issue #80.
    auto recordFailure = [&](size_t bufIdx, const std::string& srcLine,
                              const std::string& reason) {
        ++result.failures;
        int lineNum = static_cast<int>(bufIdx) + result.srcLineOffset;
        result.failureList.push_back({lineNum, srcLine, reason});
        if (result.firstFail.empty()) {
            result.firstFail = reason;
            result.failLine = srcLine;
        }
        // Stream errors to stderr in batch mode so they don't get buried
        // by stdout redirection or collapsed into the final lastMsg line
        // (which was previously routed to stdout for single-failure runs).
        // main.cpp suppresses its post-run failureList dump when headless.
        // Issue #90.
        if (isHeadless()) {
            std::fprintf(stderr, "%s\n",
                         result.formatFailure(result.failureList.back()).c_str());
        }
    };
    // Execute a single non-control line (the original runOne lambda).
    // bufIdx is the index of the source line in `lines`; srcLine is the
    // original (pre-`;`-split, pre-expand) text — both go into the
    // failure record so reports cite the *file* line, not the synthetic
    // semicolon-split fragment.
    auto runOne = [&](std::string cmd, size_t bufIdx, const std::string& srcLine) -> bool {
        size_t start = cmd.find_first_not_of(" \t");
        if (start == std::string::npos || cmd[start] == '#') return true;
        cmd.erase(0, start);
        if (!cmd.empty() && cmd[0] == ':') {
            cmd.erase(0, 1);
            size_t s2 = cmd.find_first_not_of(" \t");
            if (s2 == std::string::npos) return true;
            cmd.erase(0, s2);
        }
        size_t end = cmd.find_last_not_of(" \t");
        if (end == std::string::npos) return true;
        cmd.resize(end + 1);
        cmd = app_.expandScriptVars(cmd);
        // User-defined function call? If the first token names a :def function,
        // run its body in a fresh local frame with params seeded from the call
        // args (as env vars) instead of dispatching to a built-in command.
        {
            size_t sp = cmd.find_first_of(" \t");
            std::string fname = cmd.substr(0, sp);
            auto fit = userScriptFns_.find(fname);
            if (fit != userScriptFns_.end()) {
                std::vector<std::string> callArgs;
                if (sp != std::string::npos) {
                    std::istringstream iss(cmd.substr(sp));
                    std::string tok;
                    while (iss >> tok) callArgs.push_back(tok);
                }
                if (++scriptFnDepth_ > 64) {
                    --scriptFnDepth_;
                    recordFailure(bufIdx, srcLine, "script function recursion too deep: " + fname);
                    if (strict) { result.stopped = true; return false; }
                    return true;
                }
                std::unordered_map<std::string, std::string> seed;
                for (size_t k = 0; k < fit->second.params.size(); ++k)
                    seed[fit->second.params[k]] = k < callArgs.size() ? callArgs[k] : "";
                const std::vector<std::string> fnBody = fit->second.body;  // stable copy
                Application::ScriptFrameGuard guard(app_, true, seed, {});
                ScriptFlow ff = dispatch(fnBody, 0, fnBody.size(), result, strict);
                --scriptFnDepth_;
                return ff != ScriptFlow::Stopped;
            }
        }
        ExecResult r = app_.cmdRegistry().execute(app_, cmd);
        ++result.count;
        if (!r.msg.empty()) result.lastMsg = r.msg;
        if (!r.ok) {
            recordFailure(bufIdx, srcLine.empty() ? cmd : srcLine, r.msg);
            if (strict) { result.stopped = true; return false; }
        } else if (!r.msg.empty() && isHeadless()) {
            // Stream success messages to stdout so multi-line outputs
            // like `:objects` and `:registers` reach the user instead
            // of being clobbered by the next command's result. TUI mode
            // surfaces them via cmdLine.setMessage. Issue #90.
            std::fwrite(r.msg.data(), 1, r.msg.size(), stdout);
            std::fputc('\n', stdout);
            std::fflush(stdout);
        }
        return true;
    };
    // Find the next `;` outside of "..."/'...' quotes, so a literal
    // semicolon inside a quoted label/measure caption isn't treated as
    // a command separator (issue #87). stripScriptComment uses the same
    // quote-tracking rule for `#`.
    auto findUnquotedSemicolon = [](const std::string& s, size_t from) -> size_t {
        bool inQuote = false;
        char qc = '\0';
        for (size_t i = from; i < s.size(); ++i) {
            char c = s[i];
            if (inQuote) {
                if (c == qc) inQuote = false;
            } else if (c == '"' || c == '\'') {
                inQuote = true; qc = c;
            } else if (c == ';') {
                return i;
            }
        }
        return std::string::npos;
    };
    auto runLine = [&](std::string line, size_t bufIdx) -> bool {
        const std::string srcLine = line;
        stripScriptComment(line);
        size_t pos = 0;
        while (pos <= line.size()) {
            size_t next = findUnquotedSemicolon(line, pos);
            if (next == std::string::npos) next = line.size();
            if (!runOne(line.substr(pos, next - pos), bufIdx, srcLine)) return false;
            pos = next + 1;
        }
        return true;
    };

    // Find the matching close for a control block. Handles nesting:
    // each `openKw` line increments depth, each `closeKw` decrements.
    // On unmatched (depth never returns to 0) the caller logs an
    // "unmatched :foreach / :if" error and treats the rest of the
    // range as the body, so a missing :endif doesn't silently swallow
    // commands that were meant to run unconditionally.
    auto findMatchingClose = [&](size_t startIdx, const char* openKw,
                                  const char* closeKw, bool& matched) -> size_t {
        int depth = 1;
        std::string body;
        for (size_t i = startIdx + 1; i < hi; ++i) {
            if (isControlKeyword(lines[i], openKw,  body)) ++depth;
            else if (isControlKeyword(lines[i], closeKw, body)) {
                --depth;
                if (depth == 0) { matched = true; return i; }
            }
        }
        matched = false;
        return hi;
    };

    // Evaluate a comparison expression `LHS OP RHS` using the existing
    // register expression evaluator on each side (so `$reg.x`, math,
    // and pos() all work). Returns nullopt on parse / type failure.
    auto evalCondition = [&](const std::string& expr) -> std::optional<bool> {
        static const char* kOps[] = {"==", "!=", "<=", ">=", "<", ">"};
        for (const char* op : kOps) {
            size_t p = expr.find(op);
            if (p == std::string::npos) continue;
            std::string lhs = expr.substr(0, p);
            std::string rhs = expr.substr(p + std::strlen(op));
            trimWhitespace(lhs);
            trimWhitespace(rhs);
            // Same fully-wired context as :let, so selection-based builtins
            // (centroid/com/rmsd/pca/helix_axis/superpose_axis) work inside
            // conditionals too (previously only `regs` was wired here).
            RegisterExpr::Context ctx = makeExprContext(app_);
            auto lr = RegisterExpr::eval(lhs, ctx);
            auto rr = RegisterExpr::eval(rhs, ctx);
            if (!lr.ok || !rr.ok) return std::nullopt;
            if (lr.value.kind != Register::Kind::Scalar ||
                rr.value.kind != Register::Kind::Scalar) return std::nullopt;
            double l = lr.value.scalar, r = rr.value.scalar;
            if      (std::strcmp(op, "==") == 0) return l == r;
            else if (std::strcmp(op, "!=") == 0) return l != r;
            else if (std::strcmp(op, "<=") == 0) return l <= r;
            else if (std::strcmp(op, ">=") == 0) return l >= r;
            else if (std::strcmp(op, "<")  == 0) return l <  r;
            else                                  return l >  r;
        }
        return std::nullopt;
    };

    // Block stack — one entry per open :if. `active` is true while the
    // current branch should execute its body lines; `anyTaken` is true
    // once any branch (if / elseif / else) has been entered, so
    // subsequent elseif/else know to stay inactive.
    struct IfFrame { bool active; bool anyTaken; };
    std::vector<IfFrame> ifStack;
    auto anyInactive = [&]() {
        for (const auto& f : ifStack) if (!f.active) return true;
        return false;
    };

    std::string body;
    for (size_t i = lo; i < hi; ++i) {
        const std::string& srcLine = lines[i];

        if (isControlKeyword(srcLine, "if", body)) {
            if (anyInactive()) {
                // Still track so :endif pops correctly; subsequent
                // iterations skip the body automatically via
                // anyInactive() since active=false.
                ifStack.push_back({false, false});
                continue;
            }
            std::string expanded = app_.expandScriptVars(body);
            auto cond = evalCondition(expanded);
            if (!cond) {
                recordFailure(i, srcLine, "bad :if condition: " + expanded);
                if (strict) { result.stopped = true; return Flow::Stopped; }
                ifStack.push_back({false, false});
            } else {
                ifStack.push_back({*cond, *cond});
            }
            continue;
        }
        if (isControlKeyword(srcLine, "elseif", body)) {
            if (ifStack.empty()) continue;
            if (ifStack.back().anyTaken) {
                ifStack.back().active = false;
            } else {
                std::string expanded = app_.expandScriptVars(body);
                auto cond = evalCondition(expanded);
                bool c = cond.value_or(false);
                ifStack.back().active = c;
                if (c) ifStack.back().anyTaken = true;
            }
            continue;
        }
        if (isControlKeyword(srcLine, "else", body)) {
            if (ifStack.empty()) continue;
            ifStack.back().active = !ifStack.back().anyTaken;
            ifStack.back().anyTaken = true;
            continue;
        }
        if (isControlKeyword(srcLine, "endif", body)) {
            if (!ifStack.empty()) ifStack.pop_back();
            continue;
        }
        if (isControlKeyword(srcLine, "foreach", body)) {
            bool matched = false;
            size_t endIdx = findMatchingClose(i, "foreach", "end", matched);
            if (!matched) {
                recordFailure(i, srcLine, "unmatched :foreach (no :end found)");
                if (strict) { result.stopped = true; return Flow::Stopped; }
                i = endIdx;
                continue;
            }
            if (anyInactive()) { i = endIdx; continue; }
            // Parse `VAR in LO..HI`. expandScriptVars first so the
            // bounds can come from registers / env vars.
            std::string expanded = app_.expandScriptVars(body);
            auto inPos = expanded.find(" in ");
            if (inPos == std::string::npos) {
                recordFailure(i, srcLine, "bad :foreach header (expected `VAR in LO..HI`): " + expanded);
                if (strict) { result.stopped = true; return Flow::Stopped; }
                i = endIdx;
                continue;
            }
            std::string var = expanded.substr(0, inPos);
            std::string rangeExpr = expanded.substr(inPos + 4);
            trimWhitespace(var);
            if (var.empty()) { i = endIdx; continue; }
            // `LO..HI` → numeric range. Otherwise the tail is a selection and
            // we iterate its residues, binding ${var} (a `chain:resi` spec
            // usable in pos()), ${var_chain}, ${var_resi}, ${var_resn}.
            auto dotPos = rangeExpr.find("..");
            if (dotPos != std::string::npos) {
                int loVal = 0, hiVal = 0;
                try {
                    loVal = std::stoi(rangeExpr.substr(0, dotPos));
                    hiVal = std::stoi(rangeExpr.substr(dotPos + 2));
                } catch (const std::exception&) {
                    recordFailure(i, srcLine, "bad :foreach numeric range: " + rangeExpr);
                    if (strict) { result.stopped = true; return Flow::Stopped; }
                    i = endIdx;
                    continue;
                }
                // Scope the loop variable: save any prior binding and restore
                // it after the loop so `$var` doesn't leak into the enclosing
                // scope.
                auto prior = app_.registers().find(var);
                bool hadPrior = prior != app_.registers().end();
                Register priorVal = hadPrior ? prior->second : Register{};
                Flow outFlow = Flow::Normal;
                for (int v = loVal; v <= hiVal; ++v) {
                    Register r; r.kind = Register::Kind::Scalar; r.scalar = v;
                    app_.registers()[var] = r;
                    Flow f = dispatch(lines, i + 1, endIdx, result, strict);
                    if (f == Flow::Continue) continue;
                    if (f == Flow::Break) break;
                    if (f == Flow::Stopped || f == Flow::Return) { outFlow = f; break; }
                }
                if (hadPrior) app_.registers()[var] = priorVal;
                else          app_.registers().erase(var);
                if (outFlow != Flow::Normal) return outFlow;
                i = endIdx;
                continue;
            }
            // Selection form: iterate distinct residues. rangeExpr is already
            // ${}-expanded, so `chain ${CH}` resolves before parsing.
            auto fobj = app_.tabs().currentTab().currentObject();
            if (!fobj) {
                recordFailure(i, srcLine, ":foreach over a selection needs a current object");
                if (strict) { result.stopped = true; return Flow::Stopped; }
                i = endIdx;
                continue;
            }
            Selection fsel = app_.parseSelection(rangeExpr, *fobj);
            // Atoms of a residue are contiguous, so group consecutive picks by
            // (chain, resSeq, insCode) to get distinct residues in order.
            struct ResKey { std::string chain, resn; int resSeq; char ins; };
            std::vector<ResKey> residues;
            for (int idx : fsel.indices()) {
                if (idx < 0 || idx >= static_cast<int>(fobj->atoms().size())) continue;
                const auto& a = fobj->atoms()[idx];
                if (!residues.empty() && residues.back().chain == a.chainId &&
                    residues.back().resSeq == a.resSeq && residues.back().ins == a.insCode)
                    continue;
                residues.push_back({a.chainId, a.resName, a.resSeq, a.insCode});
            }
            // Save/restore the four bound env keys around the loop.
            const std::string keys[] = {var, var + "_chain", var + "_resi", var + "_resn"};
            std::unordered_map<std::string, std::optional<std::string>> saved;
            for (const auto& k : keys) {
                auto it = app_.scriptEnv().find(k);
                saved[k] = it != app_.scriptEnv().end()
                    ? std::optional<std::string>(it->second) : std::nullopt;
            }
            Flow outFlow = Flow::Normal;
            for (const auto& rk : residues) {
                std::string resi = std::to_string(rk.resSeq);
                app_.scriptEnv()[var]            = rk.chain + ":" + resi;
                app_.scriptEnv()[var + "_chain"] = rk.chain;
                app_.scriptEnv()[var + "_resi"]  = resi;
                app_.scriptEnv()[var + "_resn"]  = rk.resn;
                Flow f = dispatch(lines, i + 1, endIdx, result, strict);
                if (f == Flow::Continue) continue;
                if (f == Flow::Break) break;
                if (f == Flow::Stopped || f == Flow::Return) { outFlow = f; break; }
            }
            for (const auto& [k, v] : saved) {
                if (v) app_.scriptEnv()[k] = *v;
                else   app_.scriptEnv().erase(k);
            }
            if (outFlow != Flow::Normal) return outFlow;
            i = endIdx;       // past the :end
            continue;
        }
        if (isControlKeyword(srcLine, "end", body)) {
            // Stray :end (foreach consumed its own); ignore.
            continue;
        }
        // `:def name(p1, p2) ... :enddef` — capture the body (without running
        // it) and register a callable function. The header is NOT expanded:
        // name and params are literal identifiers.
        if (isControlKeyword(srcLine, "def", body)) {
            bool matched = false;
            size_t endIdx = findMatchingClose(i, "def", "enddef", matched);
            if (!matched) {
                recordFailure(i, srcLine, "unmatched :def (no :enddef found)");
                if (strict) { result.stopped = true; return Flow::Stopped; }
                i = endIdx;
                continue;
            }
            if (anyInactive()) { i = endIdx; continue; }
            std::string fname;
            std::vector<std::string> params;
            auto lp = body.find('(');
            if (lp != std::string::npos) {
                fname = body.substr(0, lp);
                auto rp = body.find(')', lp);
                std::string inside = body.substr(lp + 1,
                    rp == std::string::npos ? std::string::npos : rp - lp - 1);
                size_t p = 0;
                while (p <= inside.size()) {
                    size_t comma = inside.find(',', p);
                    std::string tok = inside.substr(p, comma == std::string::npos ? std::string::npos : comma - p);
                    trimWhitespace(tok);
                    if (!tok.empty()) params.push_back(tok);
                    if (comma == std::string::npos) break;
                    p = comma + 1;
                }
            } else {
                std::istringstream iss(body);
                iss >> fname;
                std::string p;
                while (iss >> p) params.push_back(p);
            }
            trimWhitespace(fname);
            if (!isBareName(fname)) {
                recordFailure(i, srcLine, "bad :def name: " + fname);
                if (strict) { result.stopped = true; return Flow::Stopped; }
                i = endIdx;
                continue;
            }
            UserScriptFn fn;
            fn.params = std::move(params);
            for (size_t k = i + 1; k < endIdx; ++k) fn.body.push_back(lines[k]);
            userScriptFns_[fname] = std::move(fn);
            i = endIdx;
            continue;
        }
        if (isControlKeyword(srcLine, "enddef", body)) {
            // Stray :enddef (def consumed its own); ignore.
            continue;
        }
        // Loop control — only act when no enclosing :if branch is inactive,
        // so `:if cond / :break / :endif` honors the condition. They unwind to
        // the nearest :foreach (break/continue) or the script frame (return).
        if (isControlKeyword(srcLine, "break", body)) {
            if (anyInactive()) continue;
            return Flow::Break;
        }
        if (isControlKeyword(srcLine, "continue", body)) {
            if (anyInactive()) continue;
            return Flow::Continue;
        }
        if (isControlKeyword(srcLine, "return", body)) {
            if (anyInactive()) continue;
            return Flow::Return;
        }

        // Non-control line. Skip if any enclosing :if branch is inactive.
        if (anyInactive()) continue;
        if (!runLine(srcLine, i)) return Flow::Stopped;
    }
    return Flow::Normal;
}

} // namespace molterm
