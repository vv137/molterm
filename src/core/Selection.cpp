#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <stdexcept>

#include "molterm/core/Selection.h"
#include "molterm/core/MolObject.h"

namespace molterm {

Selection::Selection(std::vector<int> indices, std::string expr)
    : indices_(std::move(indices)), expr_(std::move(expr)) {
    std::sort(indices_.begin(), indices_.end());
}

Selection Selection::operator&(const Selection& other) const {
    std::vector<int> result;
    std::set_intersection(indices_.begin(), indices_.end(),
                          other.indices_.begin(), other.indices_.end(),
                          std::back_inserter(result));
    return Selection(std::move(result), "(" + expr_ + " and " + other.expr_ + ")");
}

Selection Selection::operator|(const Selection& other) const {
    std::vector<int> result;
    std::set_union(indices_.begin(), indices_.end(),
                   other.indices_.begin(), other.indices_.end(),
                   std::back_inserter(result));
    return Selection(std::move(result), "(" + expr_ + " or " + other.expr_ + ")");
}

Selection Selection::complement(int totalAtoms) const {
    std::set<int> excluded(indices_.begin(), indices_.end());
    std::vector<int> result;
    for (int i = 0; i < totalAtoms; ++i) {
        if (!excluded.count(i)) result.push_back(i);
    }
    return Selection(std::move(result), "(not " + expr_ + ")");
}

Selection Selection::operator~() const {
    // Can't compute complement without knowing total atoms
    // Use complement(n) instead
    return *this;
}

Selection Selection::all(int totalAtoms) {
    std::vector<int> indices(totalAtoms);
    for (int i = 0; i < totalAtoms; ++i) indices[i] = i;
    return Selection(std::move(indices), "all");
}

Selection Selection::fromPredicate(const MolObject& mol,
                                    const std::function<bool(int, const AtomData&)>& pred,
                                    const std::string& expr) {
    std::vector<int> indices;
    const auto& atoms = mol.atoms();
    for (int i = 0; i < static_cast<int>(atoms.size()); ++i) {
        if (pred(i, atoms[i])) indices.push_back(i);
    }
    return Selection(std::move(indices), expr);
}

// ── Tokenizer ───────────────────────────────────────────────────────────────

namespace {

struct Token {
    enum Type { Word, Number, Dash, LParen, RParen, AtRef, End };
    Type type;
    std::string value;
};

class Tokenizer {
public:
    explicit Tokenizer(const std::string& input) : input_(input), pos_(0) {}

    Token next() {
        skipWhitespace();
        if (pos_ >= input_.size()) return {Token::End, ""};

        char ch = input_[pos_];
        if (ch == '(') { ++pos_; return {Token::LParen, "("}; }
        if (ch == ')') { ++pos_; return {Token::RParen, ")"}; }
        if (ch == '-' && pos_ + 1 < input_.size() && std::isdigit(input_[pos_+1])) {
            // Could be negative number or range dash
            // Check if previous token was a number — then it's a range dash
            return {Token::Dash, "-"};
        }
        if (ch == '-') { ++pos_; return {Token::Dash, "-"}; }

        if (ch == '@') {
            ++pos_;
            // Read the name after @
            auto word = readWord();
            return {Token::AtRef, word.value};
        }

        if (std::isdigit(ch)) {
            return readNumber();
        }
        if (std::isalpha(ch) || ch == '_' || ch == '*') {
            return readWord();
        }

        ++pos_;
        return {Token::Word, std::string(1, ch)};
    }

    Token peek() {
        size_t saved = pos_;
        auto tok = next();
        pos_ = saved;
        return tok;
    }

private:
    std::string input_;
    size_t pos_;

    void skipWhitespace() {
        while (pos_ < input_.size() && std::isspace(input_[pos_])) ++pos_;
    }

    Token readNumber() {
        size_t start = pos_;
        while (pos_ < input_.size() && std::isdigit(input_[pos_])) ++pos_;
        return {Token::Number, input_.substr(start, pos_ - start)};
    }

    Token readWord() {
        size_t start = pos_;
        while (pos_ < input_.size() &&
               (std::isalnum(input_[pos_]) || input_[pos_] == '_' || input_[pos_] == '*'))
            ++pos_;
        return {Token::Word, input_.substr(start, pos_ - start)};
    }
};

// ── Recursive descent parser ────────────────────────────────────────────────

class SelectionParser {
public:
    SelectionParser(const std::string& expr, const MolObject& mol,
                    Selection::NameResolver resolver = nullptr)
        : tokenizer_(expr), mol_(mol), resolver_(std::move(resolver)),
          totalAtoms_(static_cast<int>(mol.atoms().size())) {
        advance();
    }

    Selection parse() {
        auto sel = parseOr();
        return sel;
    }

private:
    Tokenizer tokenizer_;
    const MolObject& mol_;
    Selection::NameResolver resolver_;
    int totalAtoms_;
    Token current_;

    void advance() { current_ = tokenizer_.next(); }

    bool match(Token::Type type) {
        if (current_.type == type) { advance(); return true; }
        return false;
    }

    bool matchWord(const std::string& w) {
        if (current_.type == Token::Word && current_.value == w) {
            advance(); return true;
        }
        return false;
    }

    // or: and (("or") and)*
    Selection parseOr() {
        auto left = parseAnd();
        while (current_.type == Token::Word && current_.value == "or") {
            advance();
            auto right = parseAnd();
            left = left | right;
        }
        return left;
    }

    // and: not (("and") not)*
    Selection parseAnd() {
        auto left = parseNot();
        while (current_.type == Token::Word && current_.value == "and") {
            advance();
            auto right = parseNot();
            left = left & right;
        }
        return left;
    }

    // not: "not" not | atom
    Selection parseNot() {
        if (matchWord("not")) {
            auto inner = parseNot();
            return inner.complement(totalAtoms_);
        }
        return parseAtom();
    }

    // atom: keyword | @name | "(" or ")"
    Selection parseAtom() {
        // Parentheses
        if (match(Token::LParen)) {
            auto inner = parseOr();
            match(Token::RParen);
            return inner;
        }

        // @name — reference a named selection
        if (current_.type == Token::AtRef) {
            std::string name = current_.value;
            advance();
            if (resolver_) {
                const Selection* sel = resolver_(name);
                if (sel) return *sel;
            }
            return Selection({}, "@" + name);  // unresolved → empty
        }

        if (current_.type != Token::Word)
            return Selection({}, "");

        std::string kw = current_.value;
        // Convert to lowercase for case-insensitive matching
        std::string kwLower = kw;
        std::transform(kwLower.begin(), kwLower.end(), kwLower.begin(), ::tolower);
        advance();

        if (kwLower == "all") {
            return Selection::all(totalAtoms_);
        }
        if (kwLower == "chain") {
            std::string chainId = current_.value;
            advance();
            return Selection::fromPredicate(mol_,
                [chainId](int, const AtomData& a) { return a.chainId == chainId; },
                "chain " + chainId);
        }
        if (kwLower == "resn") {
            std::string resName = current_.value;
            // Convert to uppercase for matching
            std::transform(resName.begin(), resName.end(), resName.begin(), ::toupper);
            advance();
            return Selection::fromPredicate(mol_,
                [resName](int, const AtomData& a) { return a.resName == resName; },
                "resn " + resName);
        }
        if (kwLower == "resi") {
            int start = std::stoi(current_.value);
            advance();
            // Check for range: resi 10-20
            if (current_.type == Token::Dash) {
                advance();
                int end = std::stoi(current_.value);
                advance();
                return Selection::fromPredicate(mol_,
                    [start, end](int, const AtomData& a) {
                        return a.resSeq >= start && a.resSeq <= end;
                    },
                    "resi " + std::to_string(start) + "-" + std::to_string(end));
            }
            return Selection::fromPredicate(mol_,
                [start](int, const AtomData& a) { return a.resSeq == start; },
                "resi " + std::to_string(start));
        }
        if (kwLower == "name") {
            std::string atomName = current_.value;
            std::transform(atomName.begin(), atomName.end(), atomName.begin(), ::toupper);
            advance();
            return Selection::fromPredicate(mol_,
                [atomName](int, const AtomData& a) { return a.name == atomName; },
                "name " + atomName);
        }
        if (kwLower == "element" || kwLower == "elem") {
            std::string elem = current_.value;
            // Capitalize first letter
            if (!elem.empty()) {
                elem[0] = static_cast<char>(std::toupper(elem[0]));
                for (size_t i = 1; i < elem.size(); ++i)
                    elem[i] = static_cast<char>(std::tolower(elem[i]));
            }
            advance();
            return Selection::fromPredicate(mol_,
                [elem](int, const AtomData& a) { return a.element == elem; },
                "element " + elem);
        }
        if (kwLower == "helix") {
            return Selection::fromPredicate(mol_,
                [](int, const AtomData& a) { return a.ssType == SSType::Helix; },
                "helix");
        }
        if (kwLower == "sheet") {
            return Selection::fromPredicate(mol_,
                [](int, const AtomData& a) { return a.ssType == SSType::Sheet; },
                "sheet");
        }
        if (kwLower == "loop") {
            return Selection::fromPredicate(mol_,
                [](int, const AtomData& a) { return a.ssType == SSType::Loop; },
                "loop");
        }
        if (kwLower == "backbone" || kwLower == "bb") {
            return Selection::fromPredicate(mol_,
                [](int, const AtomData& a) {
                    return a.name == "N" || a.name == "CA" || a.name == "C" || a.name == "O";
                },
                "backbone");
        }
        if (kwLower == "sidechain" || kwLower == "sc") {
            return Selection::fromPredicate(mol_,
                [](int, const AtomData& a) {
                    return a.name != "N" && a.name != "CA" && a.name != "C" && a.name != "O";
                },
                "sidechain");
        }
        if (kwLower == "hydro") {
            return Selection::fromPredicate(mol_,
                [](int, const AtomData& a) { return a.element == "H"; },
                "hydro");
        }
        if (kwLower == "water" || kwLower == "hoh" || kwLower == "sol") {
            return Selection::fromPredicate(mol_,
                [](int, const AtomData& a) {
                    return a.resName == "HOH" || a.resName == "WAT" || a.resName == "DOD";
                },
                "water");
        }
        if (kwLower == "obj") {
            // obj <name> — select all atoms if current object name matches
            std::string objName = current_.value;
            advance();
            if (mol_.name() == objName)
                return Selection::all(totalAtoms_);
            return Selection({}, "obj " + objName);  // empty if no match
        }

        // Unknown keyword — treat as empty selection
        return Selection({}, kw);
    }
};

} // anonymous namespace

Selection Selection::parse(const std::string& expr, const MolObject& mol,
                           NameResolver resolver) {
    if (expr.empty()) return Selection();
    SelectionParser parser(expr, mol, std::move(resolver));
    return parser.parse();
}

} // namespace molterm
