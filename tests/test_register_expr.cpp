// tests/test_register_expr.cpp — unit tests for the :let expression evaluator
// (RegisterExpr::eval). Drives the evaluator with a stub Context (fake atom /
// selection resolvers) so it runs without a loaded structure.
//
// Build & run via CTest:  ctest --test-dir build -L molterm -R test_register_expr

#include "molterm/cmd/Register.h"
#include "molterm/cmd/RegisterExpr.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

using molterm::Register;
using molterm::RegisterExpr;

static int passed = 0, failed = 0;

static void ok(bool cond, const std::string& what) {
    if (cond) { ++passed; std::printf("[PASS] %s\n", what.c_str()); }
    else      { ++failed; std::printf("[FAIL] %s\n", what.c_str()); }
}

// Stub context: pos(A:1:CA) -> (10,20,30); any selection -> three points on the
// x-axis at 0/2/4 (centroid (2,0,0)), each carbon (mass 12) so com == centroid.
static RegisterExpr::Context makeStubCtx(
        const std::unordered_map<std::string, Register>& regs) {
    RegisterExpr::Context ctx;
    ctx.regs = &regs;
    ctx.resolveAtomPos = [](const std::string& spec,
                            double* x, double* y, double* z) -> bool {
        if (spec == "A:1:CA") { *x = 10; *y = 20; *z = 30; return true; }
        return false;
    };
    auto fill = [](std::vector<float>* xs, std::vector<float>* ys,
                   std::vector<float>* zs) {
        for (float v : {0.0f, 2.0f, 4.0f}) { xs->push_back(v); ys->push_back(0); zs->push_back(0); }
    };
    ctx.collectSelectionXYZ = [fill](const std::string&, std::vector<float>* xs,
                                     std::vector<float>* ys, std::vector<float>* zs) -> int {
        fill(xs, ys, zs); return 3;
    };
    ctx.collectSelectionXYZMass = [fill](const std::string&, std::vector<float>* xs,
                                         std::vector<float>* ys, std::vector<float>* zs,
                                         std::vector<float>* ms) -> int {
        fill(xs, ys, zs); for (int i = 0; i < 3; ++i) ms->push_back(12.011f); return 3;
    };
    return ctx;
}

// Evaluate against an empty register table.
static RegisterExpr::Result ev(const std::string& e) {
    std::unordered_map<std::string, Register> regs;
    return RegisterExpr::eval(e, makeStubCtx(regs));
}

static bool isScalar(const RegisterExpr::Result& r, double want, double tol = 1e-6) {
    return r.ok && r.value.kind == Register::Kind::Scalar &&
           std::abs(r.value.scalar - want) < tol;
}
static bool isVec(const RegisterExpr::Result& r, double x, double y, double z,
                  double tol = 1e-6) {
    return r.ok && r.value.kind == Register::Kind::Vec3 &&
           std::abs(r.value.vec[0] - x) < tol &&
           std::abs(r.value.vec[1] - y) < tol &&
           std::abs(r.value.vec[2] - z) < tol;
}

int main() {
    // ── Scalar arithmetic + precedence ──────────────────────────────────────
    ok(isScalar(ev("1.5 + 2 * 3"), 7.5),  "precedence: 1.5 + 2*3 = 7.5");
    ok(isScalar(ev("(1 + 2) * 3"), 9.0),  "parens: (1+2)*3 = 9");
    ok(isScalar(ev("-4 + 1"), -3.0),      "unary minus: -4 + 1 = -3");
    ok(isScalar(ev("10 / 4"), 2.5),       "division: 10/4 = 2.5");

    // ── Vec3 literals — the comma case Phase 1 makes flow through verbatim ──
    ok(isVec(ev("[1, 2, 3]"), 1, 2, 3),            "vec literal [1, 2, 3]");
    ok(isVec(ev("[1,0,0] + [0,1,0]"), 1, 1, 0),    "vec add");
    ok(isVec(ev("2 * [1, 2, 3]"), 2, 4, 6),        "scalar*vec");
    ok(isVec(ev("[2, 4, 6] / 2"), 1, 2, 3),        "vec/scalar");

    // ── Vector builtins ─────────────────────────────────────────────────────
    ok(isScalar(ev("dot([1,2,3], [4,5,6])"), 32.0),       "dot");
    ok(isVec(ev("cross([1,0,0], [0,1,0])"), 0, 0, 1),     "cross");
    ok(isScalar(ev("length([3,4,0])"), 5.0),              "length");
    ok(isScalar(ev("distance([0,0,0], [3,4,0])"), 5.0),   "distance");
    ok(isScalar(ev("length(normalize([3,4,0]))"), 1.0),   "normalize -> unit");
    ok(isScalar(ev("angle([1,0,0], [0,1,0])"), 90.0),     "angle = 90deg");

    // ── Selection / atom builtins via stubs ─────────────────────────────────
    ok(isVec(ev("pos(A:1:CA)"), 10, 20, 30),   "pos(A:1:CA)");
    ok(isVec(ev("centroid(all)"), 2, 0, 0),    "centroid of stub points");
    ok(isVec(ev("com(all)"), 2, 0, 0),         "com of equal-mass stub points");

    // ── Register refs + field access ────────────────────────────────────────
    {
        std::unordered_map<std::string, Register> regs;
        Register s; s.kind = Register::Kind::Scalar; s.scalar = 2.5; regs["s"] = s;
        Register v; v.kind = Register::Kind::Vec3; v.vec = {3, 4, 0}; regs["v"] = v;
        auto ctx = makeStubCtx(regs);
        ok(isScalar(RegisterExpr::eval("$s * 2", ctx), 5.0),   "$s * 2 = 5");
        ok(isScalar(RegisterExpr::eval("$v.x", ctx), 3.0),     "$v.x = 3");
        ok(isScalar(RegisterExpr::eval("$v.length", ctx), 5.0),"$v.length = 5");
        ok(isScalar(RegisterExpr::eval("v.y", ctx), 4.0),      "bare name v.y = 4");
    }

    // ── Error paths ─────────────────────────────────────────────────────────
    ok(!ev("[1,0,0] + 5").ok,   "type error: vec + scalar rejected");
    ok(!ev("1 / 0").ok,         "division by zero rejected");
    ok(!ev("$nope").ok,         "unknown register rejected");
    ok(!ev("pos(Z:9:CA)").ok,   "pos() no-match rejected");
    ok(!ev("[1, 2]").ok,        "malformed vec literal rejected");

    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
