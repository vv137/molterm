#include "molterm/cmd/RegisterExpr.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace molterm {

namespace {

// Lexical model lives in RegisterExpr.h next to the grammar comment.
// The lexer is independent from CommandParser's whitespace/comma split —
// `:let` rejoins after `=` and re-tokenizes here.

enum class Tok {
    End, Number, Ident, Dollar, Dot,
    LParen, RParen, LBracket, RBracket, Comma,
    Plus, Minus, Star, Slash,
};

struct Token {
    Tok kind = Tok::End;
    std::string text;   // ident / number text
    double number = 0.0;
    size_t pos = 0;     // position in source (for error reporting)
};

// Lexer that *also* knows how to grab a balanced-paren blob — used for
// pos(...) and pca(...) where the argument is a free-form atom-spec /
// selection expression we don't want to re-tokenize at this layer.
class Lexer {
public:
    explicit Lexer(const std::string& src) : src_(src) {}

    Token next() {
        skipWs();
        if (i_ >= src_.size()) return {Tok::End, "", 0.0, i_};
        size_t start = i_;
        char c = src_[i_];
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && i_ + 1 < src_.size() &&
             std::isdigit(static_cast<unsigned char>(src_[i_ + 1])))) {
            return number(start);
        }
        if (c == '_' || std::isalpha(static_cast<unsigned char>(c))) return ident(start);
        ++i_;
        switch (c) {
            case '$': return {Tok::Dollar,   "$", 0.0, start};
            case '.': return {Tok::Dot,      ".", 0.0, start};
            case '(': return {Tok::LParen,   "(", 0.0, start};
            case ')': return {Tok::RParen,   ")", 0.0, start};
            case '[': return {Tok::LBracket, "[", 0.0, start};
            case ']': return {Tok::RBracket, "]", 0.0, start};
            case ',': return {Tok::Comma,    ",", 0.0, start};
            case '+': return {Tok::Plus,     "+", 0.0, start};
            case '-': return {Tok::Minus,    "-", 0.0, start};
            case '*': return {Tok::Star,     "*", 0.0, start};
            case '/': return {Tok::Slash,    "/", 0.0, start};
        }
        return {Tok::End, std::string(1, c), 0.0, start};
    }

    // Capture the inside of the next balanced (...) group as raw text,
    // consuming it from the input. Used by pos() / pca() to pass
    // selection / atom-spec strings through to the host resolver.
    // Caller must have already consumed the opening `(`.
    std::string takeBalancedParen() {
        std::string out;
        int depth = 1;
        while (i_ < src_.size()) {
            char c = src_[i_];
            if (c == '(') ++depth;
            else if (c == ')') {
                --depth;
                if (depth == 0) { ++i_; return out; }
            }
            out += c;
            ++i_;
        }
        return out;  // unterminated — caller surfaces error
    }

    size_t pos() const { return i_; }
    void rewind(size_t p) { i_ = p; }

private:
    void skipWs() {
        while (i_ < src_.size() && std::isspace(static_cast<unsigned char>(src_[i_]))) ++i_;
    }
    Token number(size_t start) {
        std::string s;
        while (i_ < src_.size() &&
               (std::isdigit(static_cast<unsigned char>(src_[i_])) ||
                src_[i_] == '.' || src_[i_] == 'e' || src_[i_] == 'E' ||
                ((src_[i_] == '+' || src_[i_] == '-') &&
                 !s.empty() && (s.back() == 'e' || s.back() == 'E')))) {
            s += src_[i_++];
        }
        return {Tok::Number, s, std::strtod(s.c_str(), nullptr), start};
    }
    Token ident(size_t start) {
        std::string s;
        while (i_ < src_.size() &&
               (std::isalnum(static_cast<unsigned char>(src_[i_])) || src_[i_] == '_')) {
            s += src_[i_++];
        }
        return {Tok::Ident, s, 0.0, start};
    }

    const std::string& src_;
    size_t i_ = 0;
};

// ── Parser/evaluator ─────────────────────────────────────────────────────
// Single-pass: parse and evaluate as we go. No AST — registers are the
// only durable state, so an explicit AST would just be intermediate junk.
// Each value carries its kind (Scalar/Vec3/Pca) and the operator
// dispatch enforces type rules at evaluation time.

class Eval {
public:
    Eval(const std::string& src, const RegisterExpr::Context& ctx)
        : lex_(src), ctx_(ctx) { advance(); }

    RegisterExpr::Result run() {
        Register r = parseExpr();
        if (!ok_) return {false, err_, {}};
        if (cur_.kind != Tok::End) {
            return {false, "Unexpected trailing token: " + cur_.text, {}};
        }
        return {true, "", r};
    }

private:
    // ── Token stream ──
    void advance() { cur_ = lex_.next(); }
    bool accept(Tok k) {
        if (cur_.kind == k) { advance(); return true; }
        return false;
    }
    bool fail(std::string m) { ok_ = false; err_ = std::move(m); return false; }

    // ── Grammar ──
    Register parseExpr() {
        Register left = parseTerm();
        while (ok_ && (cur_.kind == Tok::Plus || cur_.kind == Tok::Minus)) {
            bool plus = (cur_.kind == Tok::Plus);
            advance();
            Register right = parseTerm();
            if (!ok_) return {};
            left = applyAdd(left, right, plus);
        }
        return left;
    }

    Register parseTerm() {
        Register left = parseFactor();
        while (ok_ && (cur_.kind == Tok::Star || cur_.kind == Tok::Slash)) {
            bool mul = (cur_.kind == Tok::Star);
            advance();
            Register right = parseFactor();
            if (!ok_) return {};
            left = applyMul(left, right, mul);
        }
        return left;
    }

    Register parseFactor() {
        if (accept(Tok::Minus)) {
            Register r = parseFactor();
            if (!ok_) return {};
            return negate(r);
        }
        return parsePrimary();
    }

    Register parsePrimary() {
        if (cur_.kind == Tok::Number) {
            Register r;
            r.kind = Register::Kind::Scalar;
            r.scalar = cur_.number;
            advance();
            return r;
        }
        if (cur_.kind == Tok::LBracket) {
            advance();
            Register x = parseExpr(); if (!ok_) return {};
            if (!accept(Tok::Comma)) { fail("expected ',' in vec3 literal"); return {}; }
            Register y = parseExpr(); if (!ok_) return {};
            if (!accept(Tok::Comma)) { fail("expected ',' in vec3 literal"); return {}; }
            Register z = parseExpr(); if (!ok_) return {};
            if (!accept(Tok::RBracket)) { fail("expected ']' to close vec3 literal"); return {}; }
            if (x.kind != Register::Kind::Scalar || y.kind != Register::Kind::Scalar ||
                z.kind != Register::Kind::Scalar) {
                fail("vec3 literal components must be scalar"); return {};
            }
            Register r;
            r.kind = Register::Kind::Vec3;
            r.vec = {x.scalar, y.scalar, z.scalar};
            return r;
        }
        if (cur_.kind == Tok::LParen) {
            advance();
            Register r = parseExpr(); if (!ok_) return {};
            if (!accept(Tok::RParen)) { fail("expected ')'"); return {}; }
            return r;
        }
        if (cur_.kind == Tok::Dollar) {
            advance();
            if (cur_.kind != Tok::Ident) { fail("expected register name after '$'"); return {}; }
            std::string name = cur_.text; advance();
            std::string field;
            if (accept(Tok::Dot)) {
                if (cur_.kind != Tok::Ident) { fail("expected field name after '.'"); return {}; }
                field = cur_.text; advance();
            }
            return resolveRegisterRef(name, field);
        }
        if (cur_.kind == Tok::Ident) {
            std::string name = cur_.text; advance();
            // ident '(' …  — function call.
            if (cur_.kind == Tok::LParen) {
                advance();
                return callBuiltin(name);
            }
            // Otherwise treat as a bare register reference: `vv_axis` is
            // sugar for `$vv_axis` so the user-written `:let d = p2 - p1`
            // resolves naturally without requiring `$` everywhere.
            std::string field;
            if (accept(Tok::Dot)) {
                if (cur_.kind != Tok::Ident) { fail("expected field name after '.'"); return {}; }
                field = cur_.text; advance();
            }
            return resolveRegisterRef(name, field);
        }
        fail("unexpected token: '" + cur_.text + "'");
        return {};
    }

    // ── Builtins ──
    // Shared capture path for the single-selection PCA-style builtins
    // (pca / helix_axis): grab the balanced-paren selection text verbatim,
    // resolve it to ordered XYZ, run `fn`, and wrap the result as a Pca
    // register. pos() and superpose_axis() differ enough to stay separate.
    using SelXYZFn = geom::PcaResult (*)(const std::vector<float>&,
                                         const std::vector<float>&,
                                         const std::vector<float>&);
    Register captureSelectionPca(const char* name, int minAtoms, SelXYZFn fn) {
        lex_.rewind(cur_.pos);
        std::string raw = lex_.takeBalancedParen();
        advance();
        if (!ctx_.collectSelectionXYZ) { fail(std::string(name) + "(): no selection resolver"); return {}; }
        std::vector<float> xs, ys, zs;
        int n = ctx_.collectSelectionXYZ(raw, &xs, &ys, &zs);
        if (n < minAtoms) {
            fail(std::string(name) + "(): need at least " + std::to_string(minAtoms) +
                 " atoms (got " + std::to_string(n) + ")");
            return {};
        }
        auto p = fn(xs, ys, zs);
        if (!p.valid) { fail(std::string(name) + "(): degenerate input"); return {}; }
        Register r; r.kind = Register::Kind::Pca; r.pca = p;
        return r;
    }

    // Shared capture path for centroid()/com(): grab the balanced-paren
    // selection text, resolve it to XYZ + per-atom mass, and reduce to a
    // single Vec3. `massWeighted` selects mass-weighting (com) vs the plain
    // geometric mean (centroid). Needs only 1 atom — unlike pca().center.
    Register captureSelectionVec3(const char* name, bool massWeighted) {
        lex_.rewind(cur_.pos);
        std::string raw = lex_.takeBalancedParen();
        advance();
        if (!ctx_.collectSelectionXYZMass) { fail(std::string(name) + "(): no selection resolver"); return {}; }
        std::vector<float> xs, ys, zs, ms;
        int n = ctx_.collectSelectionXYZMass(raw, &xs, &ys, &zs, &ms);
        if (n < 1) { fail(std::string(name) + "(): selection matched no atoms"); return {}; }
        double sx = 0, sy = 0, sz = 0, sw = 0;
        for (int i = 0; i < n; ++i) {
            double w = massWeighted ? ms[i] : 1.0;
            sx += w * xs[i]; sy += w * ys[i]; sz += w * zs[i]; sw += w;
        }
        if (sw <= 0) { fail(std::string(name) + "(): zero total weight"); return {}; }
        Register r; r.kind = Register::Kind::Vec3;
        r.vec = {sx / sw, sy / sw, sz / sw};
        return r;
    }

    // Split a balanced-paren `selA vs selB` blob (commas already swapped to
    // spaces by the :let RHS rejoin) on the first whitespace-delimited `vs`
    // token. Returns false when no such separator is present.
    static bool splitVs(const std::string& raw, std::string& selA, std::string& selB) {
        std::string norm = raw;
        for (char& c : norm) if (c == ',') c = ' ';
        for (size_t p = norm.find("vs"); p != std::string::npos; p = norm.find("vs", p + 2)) {
            bool lb = (p == 0) || std::isspace(static_cast<unsigned char>(norm[p-1]));
            bool rb = (p+2 >= norm.size()) || std::isspace(static_cast<unsigned char>(norm[p+2]));
            if (lb && rb) { selA = norm.substr(0, p); selB = norm.substr(p+2); return true; }
        }
        return false;
    }

    Register callBuiltin(const std::string& name) {
        // pos() and pca() take a free-form text argument: rewind the
        // lexer to the start of the current token (which sits just after
        // the consumed `(`), then capture the balanced-paren contents
        // verbatim and hand to the host resolver. Keeps selection /
        // atom-spec parsing OUT of the expression layer.
        if (name == "pos") {
            lex_.rewind(cur_.pos);
            std::string raw = lex_.takeBalancedParen();
            advance();  // refresh cur_ from new lexer pos
            if (!ctx_.resolveAtomPos) { fail("pos(): no atom resolver"); return {}; }
            double x, y, z;
            if (!ctx_.resolveAtomPos(raw, &x, &y, &z)) {
                fail("pos(): no atom matches '" + raw + "'"); return {};
            }
            Register r; r.kind = Register::Kind::Vec3; r.vec = {x, y, z};
            return r;
        }
        // centroid()/com() reduce a selection to one point (≥ 1 atom).
        if (name == "centroid") return captureSelectionVec3("centroid", false);
        if (name == "com")      return captureSelectionVec3("com", true);
        // pca() fits principal axes; helix_axis() fits the *helical* axis of
        // an ordered Cα/phosphate trace (robust on short/curved segments).
        if (name == "pca")        return captureSelectionPca("pca", 2, geom::pcaOf);
        if (name == "helix_axis") return captureSelectionPca("helix_axis", 3, geom::helixAxisOf);
        if (name == "superpose_axis" || name == "rmsd") {
            // Two selections separated by the `vs` keyword (the comma is
            // already claimed by the :let RHS rejoin). Each must resolve to
            // the same number of atoms, in correspondence (i-th ↔ i-th).
            // superpose_axis returns the screw axis (≥ 3 atoms); rmsd returns
            // just the non-destructive residual (≥ 1 atom).
            const bool isRmsd = (name == "rmsd");
            lex_.rewind(cur_.pos);
            std::string raw = lex_.takeBalancedParen();
            advance();
            if (!ctx_.collectSelectionXYZ) { fail(name + "(): no selection resolver"); return {}; }
            std::string selA, selB;
            if (!splitVs(raw, selA, selB)) {
                fail(name + "(selA vs selB): need two selections separated by 'vs'"); return {};
            }
            std::vector<float> axs, ays, azs, bxs, bys, bzs;
            int na = ctx_.collectSelectionXYZ(selA, &axs, &ays, &azs);
            int nb = ctx_.collectSelectionXYZ(selB, &bxs, &bys, &bzs);
            const int minA = isRmsd ? 1 : 3;
            if (na < minA || nb < minA) { fail(name + "(): each selection needs ≥ " + std::to_string(minA) + " atoms (got " + std::to_string(na) + ", " + std::to_string(nb) + ")"); return {}; }
            if (na != nb) { fail(name + "(): selections must have equal atom counts (" + std::to_string(na) + " vs " + std::to_string(nb) + ")"); return {}; }
            if (isRmsd) {
                auto rr = geom::rmsdOf(axs, ays, azs, bxs, bys, bzs);
                if (!rr.valid) { fail("rmsd(): degenerate input"); return {}; }
                Register r; r.kind = Register::Kind::Scalar; r.scalar = rr.rmsd;
                return r;
            }
            auto p = geom::superposeAxisOf(axs, ays, azs, bxs, bys, bzs);
            if (!p.valid) { fail("superpose_axis(): degenerate input"); return {}; }
            Register r; r.kind = Register::Kind::Pca; r.pca = p;
            return r;
        }
        // Generic functions take a comma-separated list of expressions.
        std::vector<Register> args;
        if (cur_.kind != Tok::RParen) {
            args.push_back(parseExpr()); if (!ok_) return {};
            while (accept(Tok::Comma)) {
                args.push_back(parseExpr()); if (!ok_) return {};
            }
        }
        if (!accept(Tok::RParen)) { fail("expected ')' in function call"); return {}; }

        if (name == "dot")       return fnDot(args);
        if (name == "cross")     return fnCross(args);
        if (name == "distance" || name == "dist")  return fnDistance(args);
        if (name == "length" || name == "len")    return fnLength(args);
        if (name == "normalize" || name == "norm") return fnNormalize(args);
        if (name == "midpoint")  return fnMidpoint(args);
        if (name == "angle")     return fnAngle(args);
        if (name == "dihedral")  return fnDihedral(args);
        // Scalar math builtins (#64). Single-arg primitives take a
        // scalar and return a scalar; two-arg primitives same. Vec3
        // input is rejected — `length(v)` already exists for vector
        // magnitudes.
        // Single-argument scalar math — table-dispatched (captureless lambdas
        // decay to function pointers). Adding a function is one table row.
        static const std::pair<const char*, double (*)(double)> kUnary[] = {
            {"abs",  [](double x){ return std::fabs(x); }},
            {"sqrt", [](double x){ return std::sqrt(x); }},
            {"exp",  [](double x){ return std::exp(x); }},
            {"log",  [](double x){ return std::log(x); }},
            {"log10",[](double x){ return std::log10(x); }},
            {"log2", [](double x){ return std::log2(x); }},
            {"sin",  [](double x){ return std::sin(x); }},
            {"cos",  [](double x){ return std::cos(x); }},
            {"tan",  [](double x){ return std::tan(x); }},
            {"asin", [](double x){ return std::asin(x); }},
            {"acos", [](double x){ return std::acos(x); }},
            {"atan", [](double x){ return std::atan(x); }},
            {"floor",[](double x){ return std::floor(x); }},
            {"ceil", [](double x){ return std::ceil(x); }},
            {"round",[](double x){ return std::round(x); }},
        };
        for (const auto& [nm, fn] : kUnary) {
            if (name != nm) continue;
            if (args.size() != 1 || args[0].kind != Register::Kind::Scalar) {
                fail(name + "(x): arg must be scalar"); return {};
            }
            Register r; r.kind = Register::Kind::Scalar; r.scalar = fn(args[0].scalar);
            return r;
        }
        // Two-argument scalar math — table-dispatched. atan2 returns degrees.
        static const std::pair<const char*, double (*)(double, double)> kBinary[] = {
            {"min",  [](double a, double b){ return std::min(a, b); }},
            {"max",  [](double a, double b){ return std::max(a, b); }},
            {"pow",  [](double a, double b){ return std::pow(a, b); }},
            {"atan2",[](double a, double b){ return std::atan2(a, b) * kRadToDeg; }},
        };
        for (const auto& [nm, fn] : kBinary) {
            if (name != nm) continue;
            if (args.size() != 2 ||
                args[0].kind != Register::Kind::Scalar ||
                args[1].kind != Register::Kind::Scalar) {
                fail(name + "(a, b): both args must be scalar"); return {};
            }
            Register r; r.kind = Register::Kind::Scalar;
            r.scalar = fn(args[0].scalar, args[1].scalar);
            return r;
        }
        fail("unknown function: " + name);
        return {};
    }

    Register fnDot(const std::vector<Register>& a) {
        if (a.size() != 2 || a[0].kind != Register::Kind::Vec3 || a[1].kind != Register::Kind::Vec3) {
            fail("dot(v1, v2): both args must be vec3"); return {};
        }
        Register r; r.kind = Register::Kind::Scalar;
        r.scalar = a[0].vec[0]*a[1].vec[0] + a[0].vec[1]*a[1].vec[1] + a[0].vec[2]*a[1].vec[2];
        return r;
    }
    Register fnCross(const std::vector<Register>& a) {
        if (a.size() != 2 || a[0].kind != Register::Kind::Vec3 || a[1].kind != Register::Kind::Vec3) {
            fail("cross(v1, v2): both args must be vec3"); return {};
        }
        Register r; r.kind = Register::Kind::Vec3;
        r.vec[0] = a[0].vec[1]*a[1].vec[2] - a[0].vec[2]*a[1].vec[1];
        r.vec[1] = a[0].vec[2]*a[1].vec[0] - a[0].vec[0]*a[1].vec[2];
        r.vec[2] = a[0].vec[0]*a[1].vec[1] - a[0].vec[1]*a[1].vec[0];
        return r;
    }
    Register fnLength(const std::vector<Register>& a) {
        if (a.size() != 1 || a[0].kind != Register::Kind::Vec3) {
            fail("length(v): arg must be vec3"); return {};
        }
        Register r; r.kind = Register::Kind::Scalar;
        r.scalar = std::sqrt(a[0].vec[0]*a[0].vec[0] + a[0].vec[1]*a[0].vec[1] + a[0].vec[2]*a[0].vec[2]);
        return r;
    }
    // distance(p1, p2) / dist(...) — |p1 − p2|. Restores the command+builtin
    // pairing :measure already had for :angle/:dihedral (issue #114).
    Register fnDistance(const std::vector<Register>& a) {
        if (a.size() != 2 || a[0].kind != Register::Kind::Vec3 || a[1].kind != Register::Kind::Vec3) {
            fail("distance(p1, p2): both args must be vec3"); return {};
        }
        double dx = a[0].vec[0]-a[1].vec[0], dy = a[0].vec[1]-a[1].vec[1], dz = a[0].vec[2]-a[1].vec[2];
        Register r; r.kind = Register::Kind::Scalar;
        r.scalar = std::sqrt(dx*dx + dy*dy + dz*dz);
        return r;
    }
    Register fnNormalize(const std::vector<Register>& a) {
        if (a.size() != 1 || a[0].kind != Register::Kind::Vec3) {
            fail("normalize(v): arg must be vec3"); return {};
        }
        double L = std::sqrt(a[0].vec[0]*a[0].vec[0] + a[0].vec[1]*a[0].vec[1] + a[0].vec[2]*a[0].vec[2]);
        if (L < 1e-12) { fail("normalize(v): zero-length vector"); return {}; }
        Register r; r.kind = Register::Kind::Vec3;
        r.vec = {a[0].vec[0]/L, a[0].vec[1]/L, a[0].vec[2]/L};
        return r;
    }
    Register fnMidpoint(const std::vector<Register>& a) {
        if (a.size() != 2 || a[0].kind != Register::Kind::Vec3 || a[1].kind != Register::Kind::Vec3) {
            fail("midpoint(p1, p2): both args must be vec3"); return {};
        }
        Register r; r.kind = Register::Kind::Vec3;
        r.vec = {0.5*(a[0].vec[0]+a[1].vec[0]),
                 0.5*(a[0].vec[1]+a[1].vec[1]),
                 0.5*(a[0].vec[2]+a[1].vec[2])};
        return r;
    }
    // File-local pi so the two angle-returning builtins (angle, atan2)
    // don't each carry their own 18-digit literal.
    static constexpr double kPi = 3.14159265358979323846;
    static constexpr double kRadToDeg = 180.0 / kPi;

    Register fnAngle(const std::vector<Register>& a) {
        if (a.size() != 2 || a[0].kind != Register::Kind::Vec3 || a[1].kind != Register::Kind::Vec3) {
            fail("angle(v1, v2): both args must be vec3"); return {};
        }
        const auto& u = a[0].vec; const auto& v = a[1].vec;
        double lu = std::sqrt(u[0]*u[0]+u[1]*u[1]+u[2]*u[2]);
        double lv = std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
        if (lu < 1e-12 || lv < 1e-12) { fail("angle(): zero-length vector"); return {}; }
        double c = (u[0]*v[0] + u[1]*v[1] + u[2]*v[2]) / (lu * lv);
        if (c >  1.0) c =  1.0;
        if (c < -1.0) c = -1.0;
        Register r; r.kind = Register::Kind::Scalar;
        r.scalar = std::acos(c) * kRadToDeg;
        return r;
    }

    Register fnDihedral(const std::vector<Register>& a) {
        if (a.size() != 4) { fail("dihedral(p1,p2,p3,p4): need 4 points"); return {}; }
        for (const auto& p : a)
            if (p.kind != Register::Kind::Vec3) { fail("dihedral(): all args must be vec3"); return {}; }
        Register r; r.kind = Register::Kind::Scalar;
        r.scalar = geom::dihedralDeg(
            a[0].vec[0], a[0].vec[1], a[0].vec[2],
            a[1].vec[0], a[1].vec[1], a[1].vec[2],
            a[2].vec[0], a[2].vec[1], a[2].vec[2],
            a[3].vec[0], a[3].vec[1], a[3].vec[2]);
        return r;
    }

    Register resolveRegisterRef(const std::string& name, const std::string& field) {
        if (!ctx_.regs) { fail("$" + name + ": no register table"); return {}; }
        auto it = ctx_.regs->find(name);
        if (it == ctx_.regs->end()) { fail("$" + name + ": no such register"); return {}; }
        const Register& src = it->second;
        if (field.empty()) return src;
        // Field access: try Vec3 first, then Scalar; promote Pca's
        // axis/center fields to Vec3 and eig1..3 to Scalar.
        if (auto v = src.getVec(field)) {
            Register r; r.kind = Register::Kind::Vec3; r.vec = *v; return r;
        }
        if (auto s = src.getScalar(field)) {
            Register r; r.kind = Register::Kind::Scalar; r.scalar = *s; return r;
        }
        fail("$" + name + "." + field + ": no such field");
        return {};
    }

    // ── Operator semantics ──
    Register negate(const Register& r) {
        Register out = r;
        if (r.kind == Register::Kind::Scalar) out.scalar = -r.scalar;
        else if (r.kind == Register::Kind::Vec3) {
            out.vec = { -r.vec[0], -r.vec[1], -r.vec[2] };
        } else { fail("unary '-' applies only to scalar/vec3"); }
        return out;
    }
    Register applyAdd(const Register& a, const Register& b, bool plus) {
        if (a.kind == Register::Kind::Scalar && b.kind == Register::Kind::Scalar) {
            Register r; r.kind = Register::Kind::Scalar;
            r.scalar = plus ? a.scalar + b.scalar : a.scalar - b.scalar;
            return r;
        }
        if (a.kind == Register::Kind::Vec3 && b.kind == Register::Kind::Vec3) {
            Register r; r.kind = Register::Kind::Vec3;
            for (int i = 0; i < 3; ++i)
                r.vec[i] = plus ? a.vec[i] + b.vec[i] : a.vec[i] - b.vec[i];
            return r;
        }
        fail("'+/-' operands must both be scalar or both be vec3");
        return {};
    }
    Register applyMul(const Register& a, const Register& b, bool mul) {
        if (a.kind == Register::Kind::Scalar && b.kind == Register::Kind::Scalar) {
            if (!mul && std::abs(b.scalar) < 1e-12) { fail("division by zero"); return {}; }
            Register r; r.kind = Register::Kind::Scalar;
            r.scalar = mul ? a.scalar * b.scalar : a.scalar / b.scalar;
            return r;
        }
        // scalar*vec, vec*scalar
        if (mul && a.kind == Register::Kind::Vec3 && b.kind == Register::Kind::Scalar) {
            Register r; r.kind = Register::Kind::Vec3;
            for (int i = 0; i < 3; ++i) r.vec[i] = a.vec[i] * b.scalar;
            return r;
        }
        if (mul && a.kind == Register::Kind::Scalar && b.kind == Register::Kind::Vec3) {
            Register r; r.kind = Register::Kind::Vec3;
            for (int i = 0; i < 3; ++i) r.vec[i] = b.vec[i] * a.scalar;
            return r;
        }
        // vec/scalar (no scalar/vec — undefined)
        if (!mul && a.kind == Register::Kind::Vec3 && b.kind == Register::Kind::Scalar) {
            if (std::abs(b.scalar) < 1e-12) { fail("division by zero"); return {}; }
            Register r; r.kind = Register::Kind::Vec3;
            for (int i = 0; i < 3; ++i) r.vec[i] = a.vec[i] / b.scalar;
            return r;
        }
        fail("'*//' operand types not supported (use dot()/cross() for vec*vec)");
        return {};
    }

    Lexer lex_;
    const RegisterExpr::Context& ctx_;
    Token cur_;
    bool ok_ = true;
    std::string err_;
};

}  // namespace

RegisterExpr::Result RegisterExpr::eval(const std::string& rhs, const Context& ctx) {
    if (rhs.empty()) return {false, "empty expression", {}};
    Eval e(rhs, ctx);
    auto r = e.run();
    r.value.expr = rhs;
    return r;
}

}  // namespace molterm
