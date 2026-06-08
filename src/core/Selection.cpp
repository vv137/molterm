#include <algorithm>
#include <cctype>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <unordered_set>

#include "molterm/core/Selection.h"
#include "molterm/core/BondTable.h"
#include "molterm/core/MolObject.h"
#include "molterm/core/SpatialHash.h"

namespace molterm {

namespace {
// Combine the object scope of two operands. An index-empty operand
// contributes no atoms, so it should not veto the other's scope (e.g.
// `1ubq/(resi 50) or 1crn/(...)` parsed against 1ubq leaves the 1crn term
// empty and the union stays scoped to 1ubq). Two non-empty operands keep a
// shared scope and drop to unscoped when they disagree.
std::string mergeScope(const Selection& a, const Selection& b) {
    if (a.empty()) return b.objectScope();
    if (b.empty()) return a.objectScope();
    return (a.objectScope() == b.objectScope()) ? a.objectScope() : std::string();
}
}  // namespace

Selection::Selection(std::vector<int> indices, std::string expr)
    : indices_(std::move(indices)), expr_(std::move(expr)) {
    std::sort(indices_.begin(), indices_.end());
}

Selection Selection::operator&(const Selection& other) const {
    std::vector<int> result;
    std::set_intersection(indices_.begin(), indices_.end(),
                          other.indices_.begin(), other.indices_.end(),
                          std::back_inserter(result));
    return Selection(std::move(result), "(" + expr_ + " and " + other.expr_ + ")")
        .setObjectScope(mergeScope(*this, other));
}

Selection Selection::operator-(const Selection& other) const {
    std::vector<int> result;
    std::set_difference(indices_.begin(), indices_.end(),
                        other.indices_.begin(), other.indices_.end(),
                        std::back_inserter(result));
    // Set-difference is a subset of the left operand, so it keeps the left
    // scope regardless of what is being subtracted.
    return Selection(std::move(result), "(" + expr_ + " minus " + other.expr_ + ")")
        .setObjectScope(objScope_);
}

Selection Selection::operator^(const Selection& other) const {
    std::vector<int> result;
    std::set_symmetric_difference(indices_.begin(), indices_.end(),
                                  other.indices_.begin(), other.indices_.end(),
                                  std::back_inserter(result));
    return Selection(std::move(result), "(" + expr_ + " xor " + other.expr_ + ")")
        .setObjectScope(mergeScope(*this, other));
}

Selection Selection::operator|(const Selection& other) const {
    std::vector<int> result;
    std::set_union(indices_.begin(), indices_.end(),
                   other.indices_.begin(), other.indices_.end(),
                   std::back_inserter(result));
    return Selection(std::move(result), "(" + expr_ + " or " + other.expr_ + ")")
        .setObjectScope(mergeScope(*this, other));
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

void Selection::addIndex(int idx) {
    auto it = std::lower_bound(indices_.begin(), indices_.end(), idx);
    if (it == indices_.end() || *it != idx)
        indices_.insert(it, idx);
}

void Selection::addIndices(const std::vector<int>& idxs) {
    if (idxs.empty()) return;
    std::vector<int> sorted = idxs;
    std::sort(sorted.begin(), sorted.end());
    std::vector<int> merged;
    merged.reserve(indices_.size() + sorted.size());
    std::set_union(indices_.begin(), indices_.end(),
                   sorted.begin(), sorted.end(),
                   std::back_inserter(merged));
    indices_ = std::move(merged);
}

void Selection::removeIndex(int idx) {
    auto it = std::lower_bound(indices_.begin(), indices_.end(), idx);
    if (it != indices_.end() && *it == idx)
        indices_.erase(it);
}

bool Selection::has(int idx) const {
    return std::binary_search(indices_.begin(), indices_.end(), idx);
}

void Selection::clear() {
    indices_.clear();
    expr_.clear();
}

Selection Selection::all(int totalAtoms) {
    std::vector<int> indices(totalAtoms);
    for (int i = 0; i < totalAtoms; ++i) indices[i] = i;
    return Selection(std::move(indices), "all");
}

bool Selection::isPrimaryKeyword(std::string_view word) {
    // Mirrors the kwLower branches in parsePrimary() below. Any keyword
    // that introduces a primary selector belongs here. Operators (and,
    // or, not, of, as) and value tokens (chain ids, residue numbers) do
    // not — only words that can stand at the start of a selection.
    static constexpr std::string_view kKeywords[] = {
        "all", "vis", "visible",
        "chain", "resn", "resname", "resi", "name",
        "element", "elem",
        "helix", "sheet", "loop",
        "backbone", "bb", "sidechain", "sc", "hydro",
        "phosphate", "sugar", "base",   // nucleic substructure (#111)
        "water", "hoh", "sol",
        "het", "hetatm", "ligand",
        "protein", "nucleic", "dna", "rna", "polymer",
        "obj", "within", "exwithin", "same",
        "byres", "bychain",          // sugar for `same residue|chain as` (#52)
        "pepseq", "seq", "sequence",
        "not",  // unary operator can lead an expression
    };
    for (auto kw : kKeywords) {
        if (word.size() != kw.size()) continue;
        bool match = true;
        for (size_t i = 0; i < kw.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(word[i])) != kw[i]) {
                match = false; break;
            }
        }
        if (match) return true;
    }
    return false;
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
    enum Type { Word, Number, Dash, LParen, RParen, AtRef, Slash, Plus, End };
    Type type;
    std::string value;
};

class Tokenizer {
public:
    explicit Tokenizer(const std::string& input) : input_(input), pos_(0) {}

    // Save / restore lookahead state so the parser can probe a multi-
    // token pattern (e.g. <word>/<lparen> for object-qualified forms)
    // and back out cheaply when it doesn't match.
    struct Pos { size_t pos; size_t lastStart; };
    Pos savePos() const { return {pos_, lastStart_}; }
    void rewind(Pos p) { pos_ = p.pos; lastStart_ = p.lastStart; }

    // True iff the current bareword (digit-led names like "1ubq" included)
    // is immediately followed by "/(". Used by the obj/(...) form because
    // "1ubq" splits into Number+Word and a token-level peek would only
    // see the Word, missing the slash beyond.
    bool peekObjSlashLParen() const {
        size_t p = lastStart_;
        while (p < input_.size() &&
               (std::isalnum(static_cast<unsigned char>(input_[p])) ||
                input_[p] == '_' || input_[p] == '*' ||
                input_[p] == '-' || input_[p] == '.')) ++p;
        return p + 1 < input_.size() && input_[p] == '/' && input_[p + 1] == '(';
    }

    Token next() {
        skipWhitespace();
        lastStart_ = pos_;
        if (pos_ >= input_.size()) return {Token::End, ""};

        char ch = input_[pos_];
        if (ch == '(') { ++pos_; return {Token::LParen, "("}; }
        if (ch == ')') { ++pos_; return {Token::RParen, ")"}; }
        if (ch == '/') { ++pos_; return {Token::Slash, "/"}; }
        if (ch == '+') { ++pos_; return {Token::Plus, "+"}; }
        if (ch == '-' && pos_ + 1 < input_.size() && std::isdigit(input_[pos_+1])) {
            // Could be negative number or range dash
            // Check if previous token was a number — then it's a range dash
            ++pos_;
            return {Token::Dash, "-"};
        }
        if (ch == '-') { ++pos_; return {Token::Dash, "-"}; }

        if (ch == '$') {
            ++pos_;
            // Read the name after $
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
        size_t savedLast = lastStart_;
        auto tok = next();
        pos_ = saved;
        lastStart_ = savedLast;
        return tok;
    }

    // Re-read the most recent token under bareword rules (digit-led
    // identifiers count as a single Word). Used by the `obj <name>`
    // handler so PDB-style names like "1ubq" parse as one token rather
    // than splitting into Number "1" + Word "ubq".
    std::string readObjName() {
        pos_ = lastStart_;
        size_t start = pos_;
        while (pos_ < input_.size() &&
               (std::isalnum(static_cast<unsigned char>(input_[pos_])) ||
                input_[pos_] == '_' || input_[pos_] == '*' ||
                input_[pos_] == '-' || input_[pos_] == '.'))
            ++pos_;
        return input_.substr(start, pos_ - start);
    }

private:
    std::string input_;
    size_t pos_;
    size_t lastStart_ = 0;

    void skipWhitespace() {
        while (pos_ < input_.size() && std::isspace(input_[pos_])) ++pos_;
    }

    Token readNumber() {
        size_t start = pos_;
        while (pos_ < input_.size() && std::isdigit(input_[pos_])) ++pos_;
        // Accept a single decimal point + fractional digits (e.g. "4.5").
        if (pos_ < input_.size() && input_[pos_] == '.' &&
            pos_ + 1 < input_.size() && std::isdigit(input_[pos_ + 1])) {
            ++pos_;
            while (pos_ < input_.size() && std::isdigit(input_[pos_])) ++pos_;
        }
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

// ── Nucleic-acid substructure helpers (issue #111) ────────────────────────────
// Partition a standard nucleotide's atoms into phosphate / sugar / base.
// PDB names the (deoxy)ribose atoms with a prime ("O5'"); some legacy files
// use a star ("O5*") instead, so prime-insensitive comparison covers both.
// The bridging O5'/O3' count as phosphate (they belong to the phosphodiester
// backbone), matching the issue's intended split and PyMOL/ChimeraX usage.
static bool naNameMatches(const std::string& name, const char* const* set, size_t n) {
    auto eqPrimeInsensitive = [](const std::string& a, const char* b) {
        size_t i = 0;
        for (; b[i] && i < a.size(); ++i) {
            char ca = a[i], cb = b[i];
            if (ca == '*') ca = '\'';
            if (cb == '*') cb = '\'';
            if (ca != cb) return false;
        }
        return b[i] == '\0' && i == a.size();
    };
    for (size_t i = 0; i < n; ++i)
        if (eqPrimeInsensitive(name, set[i])) return true;
    return false;
}
static bool isNaPhosphateAtom(const std::string& name) {
    static const char* const kSet[] = {"P", "OP1", "OP2", "OP3",
                                       "O1P", "O2P", "O3P", "O5'", "O3'"};
    return naNameMatches(name, kSet, sizeof(kSet) / sizeof(kSet[0]));
}
static bool isNaSugarAtom(const std::string& name) {
    static const char* const kSet[] = {"C1'", "C2'", "C3'", "C4'", "C5'",
                                       "O4'", "O2'"};
    return naNameMatches(name, kSet, sizeof(kSet) / sizeof(kSet[0]));
}

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
    // Left-associative parse of {or, minus, xor} at the same precedence
    // (issue #52). `A and B minus C` reads as (A and B) minus C since
    // parseAnd is one level up; `A or B minus C` reads as (A or B) minus C.
    Selection parseOr() {
        auto left = parseAnd();
        while (current_.type == Token::Word &&
               (current_.value == "or" ||
                current_.value == "minus" ||
                current_.value == "xor")) {
            std::string op = current_.value;
            advance();
            auto right = parseAnd();
            if      (op == "or")    left = left | right;
            else if (op == "minus") left = left - right;
            else                    left = left ^ right;
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

    // Parse slash notation: /obj/chain/resi/name
    // Any component can be empty (wildcard): //A/42/CA, /1abc//42, //A
    Selection parseSlash() {
        // Already consumed the first '/'
        // Read up to 4 slash-separated components
        std::vector<std::string> parts;
        std::string part;
        while (true) {
            if (current_.type == Token::Slash) {
                parts.push_back(part);
                part.clear();
                advance();
            } else if (current_.type == Token::End ||
                       current_.type == Token::LParen ||
                       current_.type == Token::RParen ||
                       (current_.type == Token::Word &&
                        (current_.value == "and" || current_.value == "or" || current_.value == "not"))) {
                parts.push_back(part);
                break;
            } else {
                part += current_.value;
                advance();
            }
        }

        // parts[0]=obj, parts[1]=chain, parts[2]=resi, parts[3]=name
        std::string objPat  = (parts.size() > 0) ? parts[0] : "";
        std::string chainPat = (parts.size() > 1) ? parts[1] : "";
        std::string resiPat  = (parts.size() > 2) ? parts[2] : "";
        std::string namePat  = (parts.size() > 3) ? parts[3] : "";

        // Parse resi pattern: "42", "10-20", "10+20+30-40", or signed forms
        // like "-5--1" (issue #63). Signed support: a leading '-' is part
        // of the start value, so the range separator is the FIRST '-'
        // *after* index 0. std::stoi is wrapped in try/catch so a
        // malformed component (e.g. "abc") yields an empty selection
        // instead of crashing the process.
        std::vector<std::pair<int,int>> resiRanges;
        if (!resiPat.empty()) {
            std::string buf;
            auto flushBuf = [&]() {
                if (buf.empty()) return;
                size_t searchFrom = (buf[0] == '-') ? 1 : 0;
                auto dash = buf.find('-', searchFrom);
                try {
                    if (dash != std::string::npos) {
                        resiRanges.push_back({std::stoi(buf.substr(0, dash)),
                                              std::stoi(buf.substr(dash + 1))});
                    } else {
                        int v = std::stoi(buf);
                        resiRanges.push_back({v, v});
                    }
                } catch (...) {
                    // skip — leave resiRanges empty for this chunk
                }
                buf.clear();
            };
            for (char c : resiPat) {
                if (c == '+') { flushBuf(); } else { buf += c; }
            }
            flushBuf();
        }

        // Parse chain pattern: could be "A", "A+B"
        std::vector<std::string> chains;
        if (!chainPat.empty()) {
            std::string cbuf;
            for (char c : chainPat) {
                if (c == '+') { if (!cbuf.empty()) chains.push_back(cbuf); cbuf.clear(); }
                else cbuf += c;
            }
            if (!cbuf.empty()) chains.push_back(cbuf);
        }

        // Parse name pattern: "CA", "CA+CB"
        std::vector<std::string> names;
        if (!namePat.empty()) {
            std::string nbuf;
            for (char c : namePat) {
                if (c == '+') { if (!nbuf.empty()) names.push_back(nbuf); nbuf.clear(); }
                else nbuf += static_cast<char>(std::toupper(c));
            }
            if (!nbuf.empty()) names.push_back(nbuf);
        }

        std::string objName = objPat;
        std::string molName = mol_.name();

        auto sel = Selection::fromPredicate(mol_,
            [objName, molName, chains, resiRanges, names](int, const AtomData& a) {
                if (!objName.empty() && molName != objName) return false;
                if (!chains.empty()) {
                    bool found = false;
                    for (const auto& c : chains) if (a.chainId == c) { found = true; break; }
                    if (!found) return false;
                }
                if (!resiRanges.empty()) {
                    bool found = false;
                    for (const auto& r : resiRanges)
                        if (a.resSeq >= r.first && a.resSeq <= r.second) { found = true; break; }
                    if (!found) return false;
                }
                if (!names.empty()) {
                    bool found = false;
                    for (const auto& n : names) if (a.name == n) { found = true; break; }
                    if (!found) return false;
                }
                return true;
            },
            "/" + objPat + "/" + chainPat + "/" + resiPat + "/" + namePat);
        // A concrete object segment scopes the result the same way the
        // `<obj>/(...)` paren form does (issue #101). Globs ("*"/"all") stay
        // unscoped so they keep matching every object.
        if (!objPat.empty() && objPat != "*" && objPat != "all")
            sel.setObjectScope(objPat);
        return sel;
    }

    Selection parseAtom() {
        // Slash notation: /obj/chain/resi/name
        if (current_.type == Token::Slash) {
            advance();
            return parseSlash();
        }

        // Object-qualified parenthesized form: <objname>/(<expr>) (issue #37).
        // Reads more naturally than `(<expr>) and obj <name>` for figure
        // scripts that need to disambiguate two objects sharing chain ids.
        // Wildcard "all/(<expr>)" / "*/(<expr>)" matches every loaded object.
        // Use the input-level peek (peekObjSlashLParen) because PDB-style
        // names like "1ubq" tokenize as Number "1" + Word "ubq" — the
        // token-level lookahead would only see the Word and miss the slash.
        if ((current_.type == Token::Word || current_.type == Token::Number) &&
            tokenizer_.peekObjSlashLParen()) {
            std::string objName = tokenizer_.readObjName();
            advance();   // refresh current_ — should be Slash
            advance();   // consume Slash → LParen
            advance();   // consume LParen
            Selection inner = parseOr();
            match(Token::RParen);
            bool wildcard = (objName == "all" || objName == "*");
            if (wildcard) return inner;                  // every object — unscoped
            // Tag the scope so the qualifier survives storage in a named
            // selection: `$name` re-expansion is then gated to this object
            // instead of leaking into every loaded object (issue #101).
            if (mol_.name() == objName) return inner.setObjectScope(objName);
            return Selection({}, objName + "/(...)").setObjectScope(objName);
        }

        // Digit-led bareword promotion: PDB-style names like "1ubq"
        // tokenize as Number "1" + Word "ubq", which would otherwise
        // bypass the Word-only keyword section below. Recover the full
        // identifier via readObjName so bare `<obj>` and `<obj>/*`
        // wildcard forms work uniformly for digit-led names. Pure
        // numeric tokens (no alpha chars) stay as Number so the
        // resi/within/imgt keyword branches still consume them. Issue #89.
        if (current_.type == Token::Number) {
            auto savedPos = tokenizer_.savePos();
            std::string id = tokenizer_.readObjName();
            bool hasAlpha = std::any_of(id.begin(), id.end(),
                [](char c){ return std::isalpha(static_cast<unsigned char>(c)); });
            if (hasAlpha) {
                current_ = Token{Token::Word, id};
            } else {
                tokenizer_.rewind(savedPos);
            }
        }

        // Parentheses
        if (match(Token::LParen)) {
            auto inner = parseOr();
            match(Token::RParen);
            return inner;
        }

        // $name — reference a named selection
        if (current_.type == Token::AtRef) {
            std::string name = current_.value;
            advance();
            if (resolver_) {
                const Selection* sel = resolver_(name);
                if (sel) {
                    // Honor a stored object scope: a named selection built
                    // from `<obj>/(...)` must not re-resolve against a
                    // different object. When scoped elsewhere it matches
                    // nothing here (issue #101).
                    if (!sel->objectScope().empty() &&
                        sel->objectScope() != mol_.name())
                        return Selection({}, "$" + name);
                    return *sel;
                }
            }
            return Selection({}, "$" + name);  // unresolved → empty
        }

        if (current_.type != Token::Word)
            return Selection({}, "");

        std::string kw = current_.value;
        // Convert to lowercase for case-insensitive matching
        std::string kwLower = kw;
        std::transform(kwLower.begin(), kwLower.end(), kwLower.begin(), ::tolower);
        advance();

        if (kwLower == "all" || kwLower == "*") {
            return Selection::all(totalAtoms_);
        }
        if (kwLower == "vis" || kwLower == "visible") {
            // Atoms visible in ANY currently-shown repr. Mirrors PyMOL's
            // `visible` / VMD's "drawn" — useful as the default subject
            // for commands like `:focus vis`.
            const MolObject* mp = &mol_;
            return Selection::fromPredicate(mol_,
                [mp](int idx, const AtomData&) {
                    if (!mp->visible()) return false;
                    static const ReprType kReprs[] = {
                        ReprType::Wireframe, ReprType::BallStick,
                        ReprType::Spacefill, ReprType::Cartoon,
                        ReprType::Ribbon,    ReprType::Backbone,
                        ReprType::Surface,
                    };
                    for (ReprType r : kReprs) {
                        if (mp->reprVisibleForAtom(r, idx)) return true;
                    }
                    return false;
                },
                "vis");
        }
        if (kwLower == "chain") {
            std::vector<std::string> ids;
            ids.push_back(current_.value);
            advance();
            while (current_.type == Token::Plus) {
                advance();
                ids.push_back(current_.value);
                advance();
            }
            return Selection::fromPredicate(mol_,
                [ids](int, const AtomData& a) {
                    for (const auto& id : ids) if (a.chainId == id) return true;
                    return false;
                },
                "chain " + ids[0]);
        }
        if (kwLower == "resn" || kwLower == "resname") {
            std::string resName = current_.value;
            // Convert to uppercase for matching
            std::transform(resName.begin(), resName.end(), resName.begin(), ::toupper);
            advance();
            return Selection::fromPredicate(mol_,
                [resName](int, const AtomData& a) { return a.resName == resName; },
                "resn " + resName);
        }
        if (kwLower == "resi") {
            // Parse: resi 10, resi 10-20, resi 10+20+30, resi 10-20+30-40
            // Negative numbers (signal-peptide-trimmed PDBs etc., issue #63)
            // tokenize as Dash + Number; parseSignedInt collapses those
            // back into a signed int. Returns nullopt on garbage so the
            // caller can skip the range entirely — falling back to 0
            // would silently match resi 0 atoms.
            std::vector<std::pair<int,int>> ranges;
            auto parseSignedInt = [&]() -> std::optional<int> {
                int sign = 1;
                if (current_.type == Token::Dash) {
                    sign = -1;
                    advance();
                }
                if (current_.type != Token::Number) return std::nullopt;
                int v = 0;
                try { v = std::stoi(current_.value); }
                catch (const std::exception&) { advance(); return std::nullopt; }
                advance();
                return v * sign;
            };
            auto parseOneResi = [&]() {
                // GCC 11 confuses bare `end` here with `std::end` via the
                // <algorithm>-pulled-in ADL set, so name the upper bound
                // explicitly to avoid the build failure on Linux release CI.
                auto start = parseSignedInt();
                std::optional<int> stop;
                bool isRange = (current_.type == Token::Dash);
                if (isRange) {
                    advance();
                    stop = parseSignedInt();
                }
                if (!start) return;          // malformed leading value — skip
                if (isRange && !stop) return;     // malformed range tail — skip
                ranges.push_back({*start, isRange ? *stop : *start});
            };
            parseOneResi();
            while (current_.type == Token::Plus) {
                advance();
                parseOneResi();
            }
            return Selection::fromPredicate(mol_,
                [ranges](int, const AtomData& a) {
                    for (const auto& r : ranges)
                        if (a.resSeq >= r.first && a.resSeq <= r.second) return true;
                    return false;
                },
                "resi");
        }
        if (kwLower == "name") {
            std::vector<std::string> names;
            std::string n = current_.value;
            std::transform(n.begin(), n.end(), n.begin(), ::toupper);
            names.push_back(n);
            advance();
            while (current_.type == Token::Plus) {
                advance();
                n = current_.value;
                std::transform(n.begin(), n.end(), n.begin(), ::toupper);
                names.push_back(n);
                advance();
            }
            return Selection::fromPredicate(mol_,
                [names](int, const AtomData& a) {
                    for (const auto& nm : names) if (a.name == nm) return true;
                    return false;
                },
                "name");
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
            // Nucleic-aware (issue #111): a nucleotide's backbone is its
            // phosphate + sugar atoms; a protein's is N/CA/C/O.
            return Selection::fromPredicate(mol_,
                [](int, const AtomData& a) {
                    if (isStandardNA(a.resName))
                        return isNaPhosphateAtom(a.name) || isNaSugarAtom(a.name);
                    return a.name == "N" || a.name == "CA" || a.name == "C" || a.name == "O";
                },
                "backbone");
        }
        if (kwLower == "sidechain" || kwLower == "sc") {
            // Complement of backbone, per residue type: for a nucleotide that
            // is the base (everything not phosphate/sugar); for a protein it
            // is everything not N/CA/C/O.
            return Selection::fromPredicate(mol_,
                [](int, const AtomData& a) {
                    if (isStandardNA(a.resName))
                        return !(isNaPhosphateAtom(a.name) || isNaSugarAtom(a.name));
                    return a.name != "N" && a.name != "CA" && a.name != "C" && a.name != "O";
                },
                "sidechain");
        }
        // Nucleic-acid substructure selectors (issue #111). Each is gated on
        // the residue being a standard nucleotide so phosphate groups in
        // ligands / protein atoms named "P" never leak in.
        if (kwLower == "phosphate") {
            return Selection::fromPredicate(mol_,
                [](int, const AtomData& a) {
                    return isStandardNA(a.resName) && isNaPhosphateAtom(a.name);
                },
                "phosphate");
        }
        if (kwLower == "sugar") {
            return Selection::fromPredicate(mol_,
                [](int, const AtomData& a) {
                    return isStandardNA(a.resName) && isNaSugarAtom(a.name);
                },
                "sugar");
        }
        if (kwLower == "base") {
            // The heavy ring atoms of a nucleotide — everything that is
            // neither phosphate nor sugar (hydrogens excluded).
            return Selection::fromPredicate(mol_,
                [](int, const AtomData& a) {
                    return isStandardNA(a.resName) && a.element != "H" &&
                           !isNaPhosphateAtom(a.name) && !isNaSugarAtom(a.name);
                },
                "base");
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
        if (kwLower == "het" || kwLower == "hetatm" || kwLower == "ligand") {
            return Selection::fromPredicate(mol_,
                [](int, const AtomData& a) { return a.isHet; },
                "het");
        }
        if (kwLower == "protein") {
            return Selection::fromPredicate(mol_,
                [](int, const AtomData& a) { return isStandardAA(a.resName); },
                "protein");
        }
        if (kwLower == "nucleic") {
            return Selection::fromPredicate(mol_,
                [](int, const AtomData& a) { return isStandardNA(a.resName); },
                "nucleic");
        }
        if (kwLower == "dna") {
            return Selection::fromPredicate(mol_,
                [](int, const AtomData& a) {
                    return a.resName == "DA" || a.resName == "DT" ||
                           a.resName == "DG" || a.resName == "DC";
                }, "dna");
        }
        if (kwLower == "rna") {
            return Selection::fromPredicate(mol_,
                [](int, const AtomData& a) {
                    return a.resName == "A" || a.resName == "U" ||
                           a.resName == "G" || a.resName == "C";
                }, "rna");
        }
        if (kwLower == "polymer") {
            return Selection::fromPredicate(mol_,
                [](int, const AtomData& a) {
                    return isStandardAA(a.resName) || isStandardNA(a.resName);
                }, "polymer");
        }
        if (kwLower == "obj") {
            // obj <name> — select all atoms if current object name matches.
            // Object names like PDB IDs ("1ubq") would otherwise tokenize
            // as Number "1" + Word "ubq", so re-read the current token as
            // a bareword to capture the full digit-led name.
            std::string objName = tokenizer_.readObjName();
            advance();
            if (mol_.name() == objName)
                return Selection::all(totalAtoms_);
            return Selection({}, "obj " + objName);  // empty if no match
        }
        if (kwLower == "imgt") {
            // imgt <region|N|N-M|N+M+...> — IMGT-canonical CDR/FR ranges
            // (issue #36) and individual positions / ranges / sets
            // (issue #84). Assumes the chain is already IMGT-numbered
            // (e.g. by an upstream ANARCI pass). Combine with `chain X`
            // to scope to a single chain: `chain A and imgt cdr3`.
            //
            // Standard IMGT numbering (imgt.org):
            //   FR1   1-26      CDR1  27-38     FR2   39-55
            //   CDR2  56-65     FR3   66-104    CDR3  105-117
            //   FR4   118-128
            // CDR ranges cover both heavy/light antibody chains and
            // TCR α/β chains identically. The `_anchored` variants
            // include the conserved framework Cys/Trp/Phe positions
            // that bookend each CDR (issue #83): cdr1_anchored = 26-39,
            // cdr2_anchored = 55-66, cdr3_anchored = 104-118.
            //
            // Numeric forms (issue #84):
            //   imgt 108        — single position
            //   imgt 105-110    — inclusive range
            //   imgt 106+108+115 — set (each token can also be N-M)
            std::vector<std::pair<int,int>> ranges;
            std::string label = "imgt";
            if (current_.type == Token::Number) {
                // Numeric form. Parse N or N-M, separated by '+'.
                while (current_.type == Token::Number) {
                    int n = 0;
                    try { n = std::stoi(current_.value); }
                    catch (...) { return Selection({}, label); }
                    advance();
                    int m = n;
                    if (current_.type == Token::Dash) {
                        advance();
                        if (current_.type != Token::Number) return Selection({}, label);
                        try { m = std::stoi(current_.value); }
                        catch (...) { return Selection({}, label); }
                        advance();
                    }
                    if (m < n) std::swap(n, m);
                    ranges.push_back({n, m});
                    label += " " + std::to_string(n);
                    if (m != n) label += "-" + std::to_string(m);
                    if (current_.type != Token::Plus) break;
                    advance();
                }
            } else {
                std::string region;
                if (current_.type == Token::Word) {
                    region = current_.value;
                    std::transform(region.begin(), region.end(), region.begin(), ::tolower);
                    advance();
                }
                int lo = 0, hi = 0;
                if      (region == "fr1")            { lo = 1;   hi = 26;  }
                else if (region == "cdr1")           { lo = 27;  hi = 38;  }
                else if (region == "cdr1_anchored")  { lo = 26;  hi = 39;  }
                else if (region == "fr2")            { lo = 39;  hi = 55;  }
                else if (region == "cdr2")           { lo = 56;  hi = 65;  }
                else if (region == "cdr2_anchored")  { lo = 55;  hi = 66;  }
                else if (region == "fr3")            { lo = 66;  hi = 104; }
                else if (region == "cdr3")           { lo = 105; hi = 117; }
                else if (region == "cdr3_anchored")  { lo = 104; hi = 118; }
                else if (region == "fr4")            { lo = 118; hi = 128; }
                else return Selection({}, "imgt " + region);  // unknown region → empty
                ranges.push_back({lo, hi});
                label = "imgt " + region;
            }
            if (ranges.empty()) return Selection({}, label);
            return Selection::fromPredicate(mol_,
                [ranges](int, const AtomData& a) {
                    for (const auto& [lo, hi] : ranges) {
                        if (a.resSeq >= lo && a.resSeq <= hi) return true;
                    }
                    return false;
                }, label);
        }
        if (kwLower == "within" || kwLower == "exwithin") {
            // within N of <subselection>   — atoms within N Å of subselection
            // exwithin N of <subselection> — same, minus the subselection
            const bool exclusive = (kwLower == "exwithin");
            if (current_.type != Token::Number) {
                return Selection({}, kwLower);   // malformed → empty
            }
            float dist = std::stof(current_.value);
            advance();
            if (!matchWord("of")) {
                return Selection({}, kwLower);   // missing "of" → empty
            }
            Selection sub = parseAtom();
            return spatialWithin(sub, dist, exclusive);
        }
        if (kwLower == "same") {
            // same KW as <subselection> — atoms sharing KW with subselection
            // KW ∈ residue, chain, resname/resn
            if (current_.type != Token::Word) {
                return Selection({}, "same");
            }
            std::string keyword = current_.value;
            std::string keywordLower = keyword;
            std::transform(keywordLower.begin(), keywordLower.end(),
                           keywordLower.begin(), ::tolower);
            advance();
            if (!matchWord("as")) {
                return Selection({}, "same " + keyword);
            }
            Selection sub = parseAtom();
            return sameAs(sub, keywordLower);
        }
        if (kwLower == "byres" || kwLower == "bychain") {
            // Sugar for `same residue|chain as <sub>` (issue #52). PyMOL/
            // ChimeraX users expect these spellings; the `same … as` form
            // stays available for the resname / resn case which has no
            // single-word alias.
            std::string shareKw = (kwLower == "byres") ? "residue" : "chain";
            Selection sub = parseAtom();
            return sameAs(sub, shareKw);
        }
        if (kwLower == "pepseq" || kwLower == "seq" || kwLower == "sequence") {
            // pepseq <pattern> — match contiguous runs of residues whose
            // one-letter codes spell the pattern. `.` and `?` are
            // single-residue wildcards. Match never crosses chain breaks.
            if (current_.type != Token::Word) {
                return Selection({}, kwLower);
            }
            std::string seq = current_.value;
            advance();
            return matchPepSeq(seq);
        }

        // `<obj>/*` and `<obj>/all` — whole-object sugar (issue #89).
        // The parenthesized `<obj>/(<expr>)` form is handled earlier by
        // peekObjSlashLParen. Any other `<obj>/<suffix>` shape is
        // malformed: return empty rather than fall through to the
        // bare-`<obj>` path below, which would silently swallow the
        // trailing tokens and report a full-object hit.
        if (current_.type == Token::Slash) {
            advance();
            std::string suffix;
            if (current_.type == Token::Word) {
                suffix = current_.value;
                advance();
            }
            if (suffix == "*" || suffix == "all") {
                if (kw == "all" || kw == "*" || mol_.name() == kw)
                    return Selection::all(totalAtoms_);
            }
            return Selection({}, kw + "/" + suffix);
        }

        // Bare `<obj>` → all atoms when the name matches mol_ (issue #89).
        // Cross-object dispatch lives in the :select fallback (#88), which
        // re-parses against each loaded object until one matches.
        if (mol_.name() == kw)
            return Selection::all(totalAtoms_);

        // Unknown keyword — treat as empty selection
        return Selection({}, kw);
    }

    // Build a Selection of atoms within `dist` Å of any atom in `sub`.
    // If `exclusive`, atoms in `sub` itself are removed from the result.
    Selection spatialWithin(const Selection& sub, float dist, bool exclusive) {
        const auto& atoms = mol_.atoms();
        if (sub.empty() || atoms.empty()) {
            return Selection({}, "");
        }
        // Cell size = dist so each query touches at most a 3×3×3 cell window.
        SpatialHash hash(dist > 0.0f ? dist : 1.0f,
                         static_cast<int>(sub.size()));
        for (int idx : sub.indices()) {
            const auto& a = atoms[idx];
            hash.insert(idx, a.x, a.y, a.z);
        }
        const float distSq = dist * dist;
        std::unordered_set<int> subSet(sub.indices().begin(), sub.indices().end());
        std::vector<int> result;
        result.reserve(atoms.size());
        for (int i = 0; i < static_cast<int>(atoms.size()); ++i) {
            const auto& a = atoms[i];
            bool hit = false;
            hash.forEachNeighbor(a.x, a.y, a.z, dist, [&](int j) {
                if (hit) return;
                const auto& b = atoms[j];
                float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
                if (dx*dx + dy*dy + dz*dz <= distSq) hit = true;
            });
            if (!hit) continue;
            if (exclusive && subSet.count(i)) continue;
            result.push_back(i);
        }
        std::ostringstream eos;
        eos << (exclusive ? "exwithin " : "within ")
            << dist << " of (" << sub.expression() << ")";
        return Selection(std::move(result), eos.str());
    }

    // Atoms sharing `keyword` (residue / chain / resname) with `sub`.
    Selection sameAs(const Selection& sub, const std::string& keyword) {
        const auto& atoms = mol_.atoms();
        if (sub.empty() || atoms.empty()) {
            return Selection({}, "same " + keyword + " as ()");
        }
        std::vector<int> result;
        std::string exprDescr = "same " + keyword + " as (" + sub.expression() + ")";

        if (keyword == "residue") {
            // (chainId, resSeq, insCode) tuple — collect all from sub
            std::set<std::tuple<std::string, int, char>> keys;
            for (int idx : sub.indices()) {
                const auto& a = atoms[idx];
                keys.emplace(a.chainId, a.resSeq, a.insCode);
            }
            for (int i = 0; i < static_cast<int>(atoms.size()); ++i) {
                const auto& a = atoms[i];
                if (keys.count({a.chainId, a.resSeq, a.insCode}))
                    result.push_back(i);
            }
        } else if (keyword == "chain") {
            std::set<std::string> chains;
            for (int idx : sub.indices()) chains.insert(atoms[idx].chainId);
            for (int i = 0; i < static_cast<int>(atoms.size()); ++i) {
                if (chains.count(atoms[i].chainId)) result.push_back(i);
            }
        } else if (keyword == "resname" || keyword == "resn") {
            std::set<std::string> resnames;
            for (int idx : sub.indices()) resnames.insert(atoms[idx].resName);
            for (int i = 0; i < static_cast<int>(atoms.size()); ++i) {
                if (resnames.count(atoms[i].resName)) result.push_back(i);
            }
        } else {
            return Selection({}, exprDescr);  // unknown keyword → empty
        }
        return Selection(std::move(result), exprDescr);
    }

    // 3-letter → 1-letter residue code. Returns '?' for unknown residues
    // (HETATM, ligand, water) so they can never match a wildcard either —
    // the surrounding pattern walk treats '?' in residue position as a
    // chain-break-equivalent.
    static char residueOneLetter(const std::string& resName) {
        static const std::unordered_map<std::string, char> table = {
            {"ALA",'A'},{"VAL",'V'},{"LEU",'L'},{"ILE",'I'},{"PRO",'P'},
            {"PHE",'F'},{"TRP",'W'},{"MET",'M'},{"GLY",'G'},{"SER",'S'},
            {"THR",'T'},{"CYS",'C'},{"TYR",'Y'},{"ASN",'N'},{"GLN",'Q'},
            {"ASP",'D'},{"GLU",'E'},{"LYS",'K'},{"ARG",'R'},{"HIS",'H'},
            {"MSE",'M'},{"SEC",'U'},{"PYL",'O'},
        };
        auto it = table.find(resName);
        return (it != table.end()) ? it->second : '?';
    }

    Selection matchPepSeq(const std::string& pattern) {
        if (pattern.empty()) return Selection({}, "pepseq ");
        std::string pat = pattern;
        for (auto& c : pat) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

        const auto& atoms = mol_.atoms();
        const int N = static_cast<int>(atoms.size());

        struct Res {
            int firstAtom, lastAtom;
            char letter;
            std::string chainId;
        };
        std::vector<Res> residues;
        int i = 0;
        while (i < N) {
            int rs = i;
            while (i < N &&
                   atoms[i].chainId == atoms[rs].chainId &&
                   atoms[i].resSeq == atoms[rs].resSeq &&
                   atoms[i].insCode == atoms[rs].insCode) ++i;
            residues.push_back({rs, i - 1,
                                residueOneLetter(atoms[rs].resName),
                                atoms[rs].chainId});
        }

        const size_t L = pat.size();
        std::vector<int> result;
        for (size_t p = 0; p + L <= residues.size(); ++p) {
            const std::string& chain = residues[p].chainId;
            bool ok = true;
            for (size_t k = 0; k < L && ok; ++k) {
                if (residues[p + k].chainId != chain) { ok = false; break; }
                char want = pat[k];
                char have = residues[p + k].letter;
                if (have == '?') { ok = false; break; }   // non-AA breaks the run
                if (want == '.' || want == '?') continue;
                if (want != have) ok = false;
            }
            if (!ok) continue;
            for (size_t k = 0; k < L; ++k) {
                for (int j = residues[p + k].firstAtom;
                     j <= residues[p + k].lastAtom; ++j) {
                    result.push_back(j);
                }
            }
        }
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return Selection(std::move(result), "pepseq " + pattern);
    }
};

} // anonymous namespace

namespace {

// Translate the `chain:resi[:name]` atom-spec (optionally `obj/chain:resi:name`)
// that pos()/pca() already accept into the slash atom-path the grammar parses,
// so :show / :color / :select / :measure(...) take the same shorthand
// uniformly (issue #106):
//   D:22:CA       -> //D/22/CA
//   D:22          -> //D/22
//   1abc/D:22:CA  -> /1abc/D/22/CA
// A ':' appears in no other selection construct, so an expression without one
// returns unchanged, and one *with* a colon currently always fails to parse —
// the rewrite can only turn a hard error into a match, never regress a working
// expression. Tokens are split on whitespace / parens; a token keeps its
// surrounding parens (e.g. `(D:22:CA)` -> `(//D/22/CA)`).
std::string normalizeColonSpecs(const std::string& expr) {
    if (expr.find(':') == std::string::npos) return expr;
    auto isTokChar = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) ||
               c == '_' || c == '*' || c == '/' || c == '.' ||
               c == '+' || c == '-' || c == ':';
    };
    std::string out;
    out.reserve(expr.size() + 8);
    size_t i = 0, n = expr.size();
    while (i < n) {
        char prev = (i == 0) ? ' ' : expr[i - 1];
        bool atBoundary = std::isspace(static_cast<unsigned char>(prev)) ||
                          prev == '(' || prev == ')';
        char c = expr[i];
        if (atBoundary &&
            (std::isalnum(static_cast<unsigned char>(c)) || c == '*' || c == '/')) {
            size_t j = i;
            while (j < n && isTokChar(expr[j])) ++j;
            std::string tok = expr.substr(i, j - i);
            if (tok.find(':') != std::string::npos) {
                bool hasObj = tok.find('/') != std::string::npos;
                for (char& tc : tok) if (tc == ':') tc = '/';
                // Slash atom-path wants a leading '/obj' (or '//' for the
                // empty-obj case); don't double a slash a token already has.
                if (tok[0] == '/')      out += tok;
                else if (hasObj)        out += "/" + tok;
                else                    out += "//" + tok;
            } else {
                out += tok;
            }
            i = j;
        } else {
            out += c;
            ++i;
        }
    }
    return out;
}

}  // namespace

Selection Selection::parse(const std::string& exprIn, const MolObject& mol,
                           NameResolver resolver) {
    if (exprIn.empty()) return Selection();
    std::string expr = normalizeColonSpecs(exprIn);
    SelectionParser parser(expr, mol, std::move(resolver));
    return parser.parse();
}

} // namespace molterm
