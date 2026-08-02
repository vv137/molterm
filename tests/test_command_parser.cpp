// tests/test_command_parser.cpp — command-line tokenizer tests
// Build & run via CTest:  ctest --test-dir build -L molterm -R test_command_parser
//
// The tokenizer is lossy by design (it eats commas so PyMOL-style
// `:color red, obj X` works), and that loss is what made issue #120 —
// `chain=A,B` split into `chain=A` + an orphaned `B` that no caller could
// reassemble. These tests pin both halves of the rule: commas separate
// arguments, except inside a KEY=VALUE argument where they belong to the value.

#include <iostream>
#include <string>
#include <vector>

#include "molterm/cmd/CommandParser.h"

using namespace molterm;

static int passed = 0;
static int failed = 0;

#define CHECK(expr, msg) \
    do { \
        if (!(expr)) { \
            std::cerr << "FAIL: " << msg << "  (" << #expr << ")" << std::endl; \
            ++failed; \
        } else { \
            ++passed; \
        } \
    } while(0)

static std::string join(const std::vector<std::string>& v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) { if (i) s += "|"; s += v[i]; }
    return s + "]";
}

// expectArgs — parse `input` and compare the arg vector to `want`.
static void expectArgs(const std::string& input,
                       const std::vector<std::string>& want) {
    auto cmd = CommandParser::parse(input);
    bool ok = (cmd.args == want);
    if (!ok) {
        std::cerr << "FAIL: `" << input << "` → " << join(cmd.args)
                  << ", want " << join(want) << std::endl;
        ++failed;
    } else {
        ++passed;
    }
}

// ── Comma as an argument separator (must not regress) ────────────────────────

static void testCommaSeparates() {
    // PyMOL-style `cmd arg, selection`, documented for :color / :show / :rename.
    expectArgs("rename old, new",              {"old", "new"});
    expectArgs("rename old,new",               {"old", "new"});
    expectArgs("color red, obj 1ubq",          {"red", "obj", "1ubq"});
    expectArgs("show cartoon, obj 2def and chain A",
               {"cartoon", "obj", "2def", "and", "chain", "A"});
    // rgb(R,G,B) still splits — no '=' in any token — and :set rejoins it.
    expectArgs("set bg rgb(32,32,32)",         {"bg", "rgb(32", "32", "32)"});
}

// ── Comma inside a KEY=VALUE value (issue #120) ──────────────────────────────

static void testCommaKeptInsideAssignment() {
    // The reported break: chain=A,B reached popModeToken as two tokens, so the
    // modifier was never recognised and its remnant corrupted the selection.
    expectArgs("align mob to ref chain=A,B",
               {"mob", "to", "ref", "chain=A,B"});
    expectArgs("align mob to ref chain=C,D,E",
               {"mob", "to", "ref", "chain=C,D,E"});
    expectArgs("align mob to ref chain1=A,B chain2=H,L",
               {"mob", "to", "ref", "chain1=A,B", "chain2=H,L"});
    expectArgs("loadalign model_?.cif chain=A,B",
               {"model_?.cif", "chain=A,B"});
    // Trailing modifiers still tokenize alongside it.
    expectArgs("align mob to ref automap chain=C,D,E",
               {"mob", "to", "ref", "automap", "chain=C,D,E"});
    // Whitespace still separates even when the previous token held an '='.
    expectArgs("run r.mt A=1,2 B=3",           {"r.mt", "A=1,2", "B=3"});
    // The '=' has to precede the comma to claim it — a bare value does not
    // inherit the exemption from an earlier argument.
    expectArgs("align mob to ref chain=A x,y", {"mob", "to", "ref", "chain=A", "x", "y"});
}

// ── Quoting and the raw tail ─────────────────────────────────────────────────

static void testQuotingAndRawArgs() {
    expectArgs("load \"my file.cif\"",         {"my file.cif"});
    // A quoted run appends to the token already in hand, so a quoted value
    // keeps its commas and stays glued to its KEY= (pre-existing behavior).
    expectArgs("run @lib/label LABEL=\"hello, world\"",
               {"@lib/label", "LABEL=hello, world"});

    // rawArgs is the verbatim tail — the escape hatch for sub-grammars.
    auto cmd = CommandParser::parse("select s = resi 1,2,3");
    CHECK(cmd.name == "select", "command name parsed");
    CHECK(cmd.rawArgs == "s = resi 1,2,3", "rawArgs keeps the tail verbatim");

    auto forced = CommandParser::parse("q!");
    CHECK(forced.name == "q" && forced.forced, "q! sets forced");
}

int main() {
    std::cout << "Running command parser tests...\n\n";
    testCommaSeparates();
    testCommaKeptInsideAssignment();
    testQuotingAndRawArgs();
    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed > 0 ? 1 : 0;
}
