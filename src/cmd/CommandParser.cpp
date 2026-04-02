#include "molterm/cmd/CommandParser.h"

#include <sstream>

namespace molterm {

ParsedCommand CommandParser::parse(const std::string& input) {
    ParsedCommand cmd;
    std::string trimmed = input;

    // Trim leading/trailing whitespace
    auto start = trimmed.find_first_not_of(" \t");
    if (start == std::string::npos) return cmd;
    trimmed = trimmed.substr(start);
    auto end = trimmed.find_last_not_of(" \t");
    if (end != std::string::npos) trimmed = trimmed.substr(0, end + 1);

    // Extract command name
    std::istringstream iss(trimmed);
    iss >> cmd.name;

    // Check for forced flag (e.g., "q!")
    if (!cmd.name.empty() && cmd.name.back() == '!') {
        cmd.forced = true;
        cmd.name.pop_back();
    }

    // Rest are args - split by comma or whitespace
    std::string rest;
    std::getline(iss, rest);

    // Trim leading whitespace from rest
    start = rest.find_first_not_of(" \t");
    if (start != std::string::npos) {
        rest = rest.substr(start);
    } else {
        rest.clear();
    }

    if (!rest.empty()) {
        // Split by whitespace/comma, but respect quotes for paths with spaces
        // :load "my file.cif"  → ["my file.cif"]
        // :set bt 2            → ["bt", "2"]
        // :rename old, new     → ["old", "new"]
        std::string token;
        bool inQuote = false;
        char quoteChar = '\0';

        for (size_t i = 0; i <= rest.size(); ++i) {
            char ch = (i < rest.size()) ? rest[i] : '\0';

            if (inQuote) {
                if (ch == quoteChar || ch == '\0') {
                    // End of quoted string
                    inQuote = false;
                    if (!token.empty()) {
                        cmd.args.push_back(token);
                        token.clear();
                    }
                } else {
                    token += ch;
                }
            } else if (ch == '"' || ch == '\'') {
                // Start quoted string
                inQuote = true;
                quoteChar = ch;
            } else if (ch == ' ' || ch == '\t' || ch == ',' || ch == '\0') {
                if (!token.empty()) {
                    cmd.args.push_back(token);
                    token.clear();
                }
            } else {
                token += ch;
            }
        }
    }

    return cmd;
}

} // namespace molterm
