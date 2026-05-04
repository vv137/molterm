#include <algorithm>
#include <cctype>
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

    Token next() {
        skipWhitespace();
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

        // Parse resi pattern: could be "42", "10-20", "10+20+30-40"
        std::vector<std::pair<int,int>> resiRanges;
        if (!resiPat.empty()) {
            // Split by +
            std::string buf;
            auto flushBuf = [&]() {
                if (buf.empty()) return;
                auto dash = buf.find('-');
                if (dash != std::string::npos && dash > 0) {
                    resiRanges.push_back({std::stoi(buf.substr(0, dash)),
                                          std::stoi(buf.substr(dash + 1))});
                } else {
                    int v = std::stoi(buf);
                    resiRanges.push_back({v, v});
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

        return Selection::fromPredicate(mol_,
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
    }

    Selection parseAtom() {
        // Slash notation: /obj/chain/resi/name
        if (current_.type == Token::Slash) {
            advance();
            return parseSlash();
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
                if (sel) return *sel;
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

        if (kwLower == "all") {
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
            // Parse: resi 10, resi 10-20, resi 10+20+30, resi 10-20+30-40
            std::vector<std::pair<int,int>> ranges;  // {start, end} pairs
            auto parseOneResi = [&]() {
                int start = std::stoi(current_.value);
                advance();
                if (current_.type == Token::Dash) {
                    advance();
                    int end = std::stoi(current_.value);
                    advance();
                    ranges.push_back({start, end});
                } else {
                    ranges.push_back({start, start});
                }
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
            // obj <name> — select all atoms if current object name matches
            std::string objName = current_.value;
            advance();
            if (mol_.name() == objName)
                return Selection::all(totalAtoms_);
            return Selection({}, "obj " + objName);  // empty if no match
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
};

} // anonymous namespace

Selection Selection::parse(const std::string& expr, const MolObject& mol,
                           NameResolver resolver) {
    if (expr.empty()) return Selection();
    SelectionParser parser(expr, mol, std::move(resolver));
    return parser.parse();
}

} // namespace molterm
