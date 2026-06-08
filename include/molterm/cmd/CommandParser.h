#pragma once

#include <string>
#include <vector>

namespace molterm {

struct ParsedCommand {
    std::string name;
    std::vector<std::string> args;
    // The untokenized argument tail (everything after the command name, with
    // leading whitespace trimmed). Commands that carry a sub-grammar the
    // whitespace/comma tokenizer would mangle — `:let`/`:select` expressions,
    // where `[1, 2, 3]` and `chain A and resi 5` mean different things — read
    // this instead of re-joining `args`.
    std::string rawArgs;
    bool forced = false;  // e.g., :q! sets forced=true
};

class CommandParser {
public:
    static ParsedCommand parse(const std::string& input);
};

} // namespace molterm
