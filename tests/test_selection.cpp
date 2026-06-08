// tests/test_selection.cpp — Selection parser unit tests
// Build & run via CTest:  ctest --test-dir build -L molterm -R test_selection

#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include <set>

#include "molterm/core/MolObject.h"
#include "molterm/core/Selection.h"

using namespace molterm;

// ── Helper: build a small fake molecule ─────────────────────────────────────

static MolObject makeMol() {
    MolObject mol("test");
    auto& atoms = mol.atoms();

    // Index 0-3: chain A, resi 10, ALA, protein backbone+sidechain
    atoms.push_back({0,0,0, "N",  "N", "ALA", "A", 10, ' ', 0, 1, 1, 0, false, SSType::Helix});
    atoms.push_back({0,0,0, "CA", "C", "ALA", "A", 10, ' ', 0, 1, 2, 0, false, SSType::Helix});
    atoms.push_back({0,0,0, "C",  "C", "ALA", "A", 10, ' ', 0, 1, 3, 0, false, SSType::Helix});
    atoms.push_back({0,0,0, "O",  "O", "ALA", "A", 10, ' ', 0, 1, 4, 0, false, SSType::Helix});

    // Index 4-7: chain A, resi 11, GLY
    atoms.push_back({0,0,0, "N",  "N", "GLY", "A", 11, ' ', 0, 1, 5, 0, false, SSType::Sheet});
    atoms.push_back({0,0,0, "CA", "C", "GLY", "A", 11, ' ', 0, 1, 6, 0, false, SSType::Sheet});
    atoms.push_back({0,0,0, "C",  "C", "GLY", "A", 11, ' ', 0, 1, 7, 0, false, SSType::Sheet});
    atoms.push_back({0,0,0, "O",  "O", "GLY", "A", 11, ' ', 0, 1, 8, 0, false, SSType::Sheet});

    // Index 8-11: chain B, resi 20, LEU
    atoms.push_back({0,0,0, "N",  "N", "LEU", "B", 20, ' ', 0, 1, 9, 0, false, SSType::Loop});
    atoms.push_back({0,0,0, "CA", "C", "LEU", "B", 20, ' ', 0, 1, 10, 0, false, SSType::Loop});
    atoms.push_back({0,0,0, "C",  "C", "LEU", "B", 20, ' ', 0, 1, 11, 0, false, SSType::Loop});
    atoms.push_back({0,0,0, "CB", "C", "LEU", "B", 20, ' ', 0, 1, 12, 0, false, SSType::Loop});

    // Index 12-13: chain H, resi 26, VAL (to match user's exact use-case)
    atoms.push_back({0,0,0, "N",  "N", "VAL", "H", 26, ' ', 0, 1, 13, 0, false, SSType::Helix});
    atoms.push_back({0,0,0, "CA", "C", "VAL", "H", 26, ' ', 0, 1, 14, 0, false, SSType::Helix});

    // Index 14-15: chain H, resi 30, VAL
    atoms.push_back({0,0,0, "N",  "N", "VAL", "H", 30, ' ', 0, 1, 15, 0, false, SSType::Helix});
    atoms.push_back({0,0,0, "CA", "C", "VAL", "H", 30, ' ', 0, 1, 16, 0, false, SSType::Helix});

    // Index 16-17: chain H, resi 35, VAL (outside 26-32)
    atoms.push_back({0,0,0, "N",  "N", "VAL", "H", 35, ' ', 0, 1, 17, 0, false, SSType::Helix});
    atoms.push_back({0,0,0, "CA", "C", "VAL", "H", 35, ' ', 0, 1, 18, 0, false, SSType::Helix});

    // Index 18: HOH water (HETATM)
    atoms.push_back({0,0,0, "O",  "O", "HOH", "A", 100, ' ', 0, 1, 19, 0, true, SSType::Loop});

    // Index 19: WAT water (HETATM)
    atoms.push_back({0,0,0, "O",  "O", "WAT", "A", 101, ' ', 0, 1, 20, 0, true, SSType::Loop});

    // Index 20: hydrogen on chain A
    atoms.push_back({0,0,0, "H",  "H", "ALA", "A", 10, ' ', 0, 1, 21, 0, false, SSType::Helix});

    // Index 21: DOD (deuterated water)
    atoms.push_back({0,0,0, "O",  "O", "DOD", "A", 102, ' ', 0, 1, 22, 0, true, SSType::Loop});

    // Index 22: ligand HETATM
    atoms.push_back({0,0,0, "C1", "C", "LIG", "C", 1, ' ', 0, 1, 23, 0, true, SSType::Loop});

    // Index 23-45: chain D, resi 5, DG (a deoxyguanosine nucleotide) — for the
    // nucleic substructure selectors (issue #111). Phosphate (5), sugar (6),
    // base heavy (11), plus one base hydrogen to prove `base` drops H while
    // nucleic `sidechain` keeps it.
    // phosphate: P, OP1, OP2, O5', O3'   (indices 23-27)
    atoms.push_back({0,0,0, "P",   "P", "DG", "D", 5, ' ', 0, 1, 24, 0, false, SSType::Loop});
    atoms.push_back({0,0,0, "OP1", "O", "DG", "D", 5, ' ', 0, 1, 25, 0, false, SSType::Loop});
    atoms.push_back({0,0,0, "OP2", "O", "DG", "D", 5, ' ', 0, 1, 26, 0, false, SSType::Loop});
    atoms.push_back({0,0,0, "O5'", "O", "DG", "D", 5, ' ', 0, 1, 27, 0, false, SSType::Loop});
    atoms.push_back({0,0,0, "O3'", "O", "DG", "D", 5, ' ', 0, 1, 28, 0, false, SSType::Loop});
    // sugar: C5', C4', O4', C3', C2', C1'   (indices 28-33)
    atoms.push_back({0,0,0, "C5'", "C", "DG", "D", 5, ' ', 0, 1, 29, 0, false, SSType::Loop});
    atoms.push_back({0,0,0, "C4'", "C", "DG", "D", 5, ' ', 0, 1, 30, 0, false, SSType::Loop});
    atoms.push_back({0,0,0, "O4'", "O", "DG", "D", 5, ' ', 0, 1, 31, 0, false, SSType::Loop});
    atoms.push_back({0,0,0, "C3'", "C", "DG", "D", 5, ' ', 0, 1, 32, 0, false, SSType::Loop});
    atoms.push_back({0,0,0, "C2'", "C", "DG", "D", 5, ' ', 0, 1, 33, 0, false, SSType::Loop});
    atoms.push_back({0,0,0, "C1'", "C", "DG", "D", 5, ' ', 0, 1, 34, 0, false, SSType::Loop});
    // base heavy: N9 C8 N7 C5 C6 O6 N1 C2 N2 N3 C4   (indices 34-44)
    for (const char* nm : {"N9","C8","N7","C5","C6","O6","N1","C2","N2","N3","C4"}) {
        std::string el(1, nm[0]);
        atoms.push_back({0,0,0, nm, el, "DG", "D", 5, ' ', 0, 1, 0, 0, false, SSType::Loop});
    }
    // base hydrogen (index 45) — not a ring heavy atom.
    atoms.push_back({0,0,0, "H1",  "H", "DG", "D", 5, ' ', 0, 1, 0, 0, false, SSType::Loop});

    return mol;
}

static std::set<int> toSet(const Selection& sel) {
    return {sel.indices().begin(), sel.indices().end()};
}

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

// ── Tests ───────────────────────────────────────────────────────────────────

static void testChainBasic(const MolObject& mol) {
    auto sel = Selection::parse("chain A", mol);
    auto s = toSet(sel);
    CHECK(s.count(0) && s.count(1) && s.count(2) && s.count(3), "chain A includes resi 10");
    CHECK(s.count(4) && s.count(5), "chain A includes resi 11");
    CHECK(s.count(18) && s.count(19), "chain A includes waters");
    CHECK(s.count(20), "chain A includes hydrogen");
    CHECK(!s.count(8), "chain A excludes chain B");
    CHECK(!s.count(12), "chain A excludes chain H");
}

static void testChainMultiple(const MolObject& mol) {
    auto sel = Selection::parse("chain A+B", mol);
    auto s = toSet(sel);
    CHECK(s.count(0), "chain A+B includes A");
    CHECK(s.count(8), "chain A+B includes B");
    CHECK(!s.count(12), "chain A+B excludes H");
}

static void testChainH(const MolObject& mol) {
    auto sel = Selection::parse("chain H", mol);
    auto s = toSet(sel);
    CHECK(s.count(12) && s.count(13), "chain H resi 26");
    CHECK(s.count(14) && s.count(15), "chain H resi 30");
    CHECK(s.count(16) && s.count(17), "chain H resi 35");
    CHECK(!s.count(0), "chain H excludes A");
    CHECK(sel.size() == 6, "chain H has 6 atoms");
}

static void testResiSingle(const MolObject& mol) {
    auto sel = Selection::parse("resi 10", mol);
    auto s = toSet(sel);
    CHECK(s.count(0) && s.count(1) && s.count(2) && s.count(3), "resi 10 finds ALA");
    CHECK(s.count(20), "resi 10 includes H atom");
    CHECK(!s.count(4), "resi 10 excludes resi 11");
}

static void testResiRange(const MolObject& mol) {
    auto sel = Selection::parse("resi 10-11", mol);
    CHECK(sel.size() == 9, "resi 10-11 = 8 backbone + 1 H");
    auto s = toSet(sel);
    CHECK(s.count(0) && s.count(7), "resi 10-11 spans both residues");
    CHECK(!s.count(8), "resi 10-11 excludes resi 20");
}

static void testResiRangeChainH(const MolObject& mol) {
    // This is the exact user-reported crash case!
    auto sel = Selection::parse("chain H and resi 26-32", mol);
    auto s = toSet(sel);
    CHECK(s.count(12) && s.count(13), "chain H resi 26 in range");
    CHECK(s.count(14) && s.count(15), "chain H resi 30 in range");
    CHECK(!s.count(16) && !s.count(17), "chain H resi 35 out of range");
    CHECK(sel.size() == 4, "chain H and resi 26-32 = 4 atoms");
}

static void testResiMultiRange(const MolObject& mol) {
    auto sel = Selection::parse("resi 10+20", mol);
    auto s = toSet(sel);
    CHECK(s.count(0), "resi 10+20 includes 10");
    CHECK(s.count(8), "resi 10+20 includes 20");
    CHECK(!s.count(4), "resi 10+20 excludes 11");
}

static void testResiMixedRange(const MolObject& mol) {
    auto sel = Selection::parse("resi 10-11+26", mol);
    auto s = toSet(sel);
    CHECK(s.count(0), "resi 10-11+26 includes 10");
    CHECK(s.count(4), "resi 10-11+26 includes 11");
    CHECK(s.count(12), "resi 10-11+26 includes 26");
    CHECK(!s.count(8), "resi 10-11+26 excludes 20");
}

static void testResn(const MolObject& mol) {
    auto sel = Selection::parse("resn ALA", mol);
    auto s = toSet(sel);
    CHECK(s.count(0) && s.count(1) && s.count(2) && s.count(3), "resn ALA");
    CHECK(s.count(20), "resn ALA includes H on ALA");
    CHECK(!s.count(4), "resn ALA excludes GLY");
}

static void testName(const MolObject& mol) {
    auto sel = Selection::parse("name CA", mol);
    auto s = toSet(sel);
    CHECK(s.count(1), "name CA finds A:10:CA");
    CHECK(s.count(5), "name CA finds A:11:CA");
    CHECK(s.count(9), "name CA finds B:20:CA");
    CHECK(!s.count(0), "name CA excludes N");
}

static void testNameMultiple(const MolObject& mol) {
    auto sel = Selection::parse("name CA+CB", mol);
    auto s = toSet(sel);
    CHECK(s.count(1), "name CA+CB includes CA");
    CHECK(s.count(11), "name CA+CB includes CB");
    CHECK(!s.count(0), "name CA+CB excludes N");
}

static void testElement(const MolObject& mol) {
    auto sel = Selection::parse("element C", mol);
    for (auto idx : sel.indices()) {
        CHECK(mol.atoms()[idx].element == "C", "element C only yields C atoms");
    }
    CHECK(sel.size() > 0, "element C is non-empty");
}

static void testAndOperator(const MolObject& mol) {
    auto sel = Selection::parse("chain A and resn GLY", mol);
    auto s = toSet(sel);
    CHECK(s.count(4) && s.count(5) && s.count(6) && s.count(7), "chain A and resn GLY");
    CHECK(!s.count(0), "and excludes non-GLY");
    CHECK(sel.size() == 4, "chain A and resn GLY = 4 atoms");
}

static void testOrOperator(const MolObject& mol) {
    auto sel = Selection::parse("chain B or chain H", mol);
    auto s = toSet(sel);
    CHECK(s.count(8), "B or H includes B");
    CHECK(s.count(12), "B or H includes H");
    CHECK(!s.count(0), "B or H excludes A");
    CHECK(sel.size() == 10, "chain B or chain H = 10 atoms");
}

static void testNotOperator(const MolObject& mol) {
    int total = static_cast<int>(mol.atoms().size());
    auto all = Selection::parse("all", mol);
    auto notA = Selection::parse("not chain A", mol);
    auto chainA = Selection::parse("chain A", mol);
    CHECK(notA.size() + chainA.size() == all.size(), "not chain A + chain A = all");
    auto s = toSet(notA);
    CHECK(!s.count(0), "not chain A excludes A atoms");
    CHECK(s.count(8), "not chain A includes B");
}

static void testParentheses(const MolObject& mol) {
    auto sel = Selection::parse("(chain A or chain B) and name CA", mol);
    auto s = toSet(sel);
    CHECK(s.count(1), "paren: A CA");
    CHECK(s.count(5), "paren: A GLY CA");
    CHECK(s.count(9), "paren: B CA");
    CHECK(!s.count(13), "paren: excludes H CA");
    CHECK(sel.size() == 3, "(chain A or chain B) and name CA = 3");
}

static void testNestedBoolean(const MolObject& mol) {
    // chain A and (resi 10 or resi 11) and name N
    auto sel = Selection::parse("chain A and (resi 10 or resi 11) and name N", mol);
    auto s = toSet(sel);
    CHECK(s.count(0), "nested: A:10:N");
    CHECK(s.count(4), "nested: A:11:N");
    CHECK(sel.size() == 2, "nested: 2 N atoms");
}

static void testNotWithAnd(const MolObject& mol) {
    auto sel = Selection::parse("chain A and not resn GLY", mol);
    auto s = toSet(sel);
    CHECK(s.count(0), "A and not GLY includes ALA");
    CHECK(!s.count(4), "A and not GLY excludes GLY");
    CHECK(s.count(18), "A and not GLY includes HOH");
}

static void testDoubleNot(const MolObject& mol) {
    auto sel = Selection::parse("not not chain A", mol);
    auto selA = Selection::parse("chain A", mol);
    CHECK(sel.size() == selA.size(), "not not chain A == chain A");
}

// ── Water / Hydro ────────────────────────────────────────────────────────────

static void testWater(const MolObject& mol) {
    auto sel = Selection::parse("water", mol);
    auto s = toSet(sel);
    CHECK(s.count(18), "water includes HOH");
    CHECK(s.count(19), "water includes WAT");
    CHECK(s.count(21), "water includes DOD");
    CHECK(sel.size() == 3, "water = 3 atoms (HOH + WAT + DOD)");
}

static void testHOH(const MolObject& mol) {
    auto sel = Selection::parse("hoh", mol);
    CHECK(sel.size() == 3, "hoh synonym = water (3 atoms)");
}

static void testSol(const MolObject& mol) {
    auto sel = Selection::parse("sol", mol);
    CHECK(sel.size() == 3, "sol synonym = water (3 atoms)");
}

static void testHydro(const MolObject& mol) {
    auto sel = Selection::parse("hydro", mol);
    auto s = toSet(sel);
    CHECK(s.count(20), "hydro includes H atom");
    // index 20 (ALA H) + index 45 (DG base H1)
    CHECK(s.count(45), "hydro includes the nucleotide base H");
    CHECK(sel.size() == 2, "hydro = 2 hydrogen atoms");
}

static void testNotWater(const MolObject& mol) {
    auto all = Selection::parse("all", mol);
    auto water = Selection::parse("water", mol);
    auto notWater = Selection::parse("not water", mol);
    CHECK(notWater.size() + water.size() == all.size(), "not water + water = all");
}

static void testProteinAndNotWater(const MolObject& mol) {
    auto sel = Selection::parse("not water and not hydro", mol);
    auto s = toSet(sel);
    CHECK(!s.count(18), "not water/hydro excludes HOH");
    CHECK(!s.count(20), "not water/hydro excludes H");
    CHECK(s.count(0), "not water/hydro includes protein N");
}

// ── SS keywords ──────────────────────────────────────────────────────────────

static void testHelix(const MolObject& mol) {
    auto sel = Selection::parse("helix", mol);
    for (auto idx : sel.indices())
        CHECK(mol.atoms()[idx].ssType == SSType::Helix, "helix only yields helix atoms");
    CHECK(sel.size() > 0, "helix is non-empty");
}

static void testSheet(const MolObject& mol) {
    auto sel = Selection::parse("sheet", mol);
    for (auto idx : sel.indices())
        CHECK(mol.atoms()[idx].ssType == SSType::Sheet, "sheet only yields sheet atoms");
}

static void testLoop(const MolObject& mol) {
    auto sel = Selection::parse("loop", mol);
    for (auto idx : sel.indices())
        CHECK(mol.atoms()[idx].ssType == SSType::Loop, "loop only yields loop atoms");
}

// ── Backbone / Sidechain ─────────────────────────────────────────────────────

static void testBackbone(const MolObject& mol) {
    auto sel = Selection::parse("backbone", mol);
    auto s = toSet(sel);
    CHECK(s.count(0), "backbone includes N");
    CHECK(s.count(1), "backbone includes CA");
    CHECK(s.count(2), "backbone includes C");
    CHECK(s.count(3), "backbone includes O");
    CHECK(!s.count(11), "backbone excludes CB");
}

static void testBB(const MolObject& mol) {
    auto sel = Selection::parse("bb", mol);
    auto sel2 = Selection::parse("backbone", mol);
    CHECK(sel.size() == sel2.size(), "bb == backbone");
}

static void testSidechain(const MolObject& mol) {
    auto sel = Selection::parse("sidechain", mol);
    auto s = toSet(sel);
    CHECK(s.count(11), "sidechain includes CB");
    CHECK(!s.count(0), "sidechain excludes N");
}

// ── Het / Protein ────────────────────────────────────────────────────────────

static void testHet(const MolObject& mol) {
    auto sel = Selection::parse("het", mol);
    for (auto idx : sel.indices())
        CHECK(mol.atoms()[idx].isHet, "het only yields HETATM");
    CHECK(sel.size() > 0, "het non-empty");
}

static void testProtein(const MolObject& mol) {
    auto sel = Selection::parse("protein", mol);
    auto s = toSet(sel);
    CHECK(s.count(0), "protein includes ALA");
    CHECK(s.count(4), "protein includes GLY");
    CHECK(s.count(8), "protein includes LEU");
    CHECK(!s.count(18), "protein excludes HOH");
}

// ── Slash notation ───────────────────────────────────────────────────────────

static void testSlashChainResi(const MolObject& mol) {
    auto sel = Selection::parse("//A/10", mol);
    auto s = toSet(sel);
    CHECK(s.count(0) && s.count(1) && s.count(2) && s.count(3), "//A/10 basic");
    CHECK(s.count(20), "//A/10 includes H");
    CHECK(!s.count(4), "//A/10 excludes resi 11");
}

static void testSlashChainResiRange(const MolObject& mol) {
    auto sel = Selection::parse("//H/26-32", mol);
    auto s = toSet(sel);
    CHECK(s.count(12) && s.count(13), "//H/26-32 includes resi 26");
    CHECK(s.count(14) && s.count(15), "//H/26-32 includes resi 30");
    CHECK(!s.count(16), "//H/26-32 excludes resi 35");
}

static void testSlashName(const MolObject& mol) {
    auto sel = Selection::parse("//A/10/CA", mol);
    auto s = toSet(sel);
    CHECK(s.count(1), "//A/10/CA finds CA");
    CHECK(sel.size() == 1, "//A/10/CA is single atom");
}

// ── All ──────────────────────────────────────────────────────────────────────

static void testAll(const MolObject& mol) {
    auto sel = Selection::parse("all", mol);
    CHECK(sel.size() == mol.atoms().size(), "all selects everything");
}

// ── Named selection ($name) ──────────────────────────────────────────────────

static void testNamedSelection(const MolObject& mol) {
    Selection named({0, 1, 2}, "mysel");
    auto resolver = [&](const std::string& n) -> const Selection* {
        if (n == "mysel") return &named;
        return nullptr;
    };
    auto sel = Selection::parse("$mysel", mol, resolver);
    CHECK(sel.size() == 3, "$mysel resolves to 3 atoms");
}

static void testNamedAndChain(const MolObject& mol) {
    Selection named({0, 1, 2, 8, 9}, "mysel");
    auto resolver = [&](const std::string& n) -> const Selection* {
        if (n == "mysel") return &named;
        return nullptr;
    };
    auto sel = Selection::parse("$mysel and chain A", mol, resolver);
    auto s = toSet(sel);
    CHECK(s.count(0) && s.count(1) && s.count(2), "$mysel and chain A");
    CHECK(!s.count(8), "$mysel and chain A excludes B");
    CHECK(sel.size() == 3, "$mysel and chain A = 3");
}

// ── Object qualifier scope (issue #101) ──────────────────────────────────────

static void testObjQualifierMatch(const MolObject& mol) {
    // mol is named "test". `test/(...)` resolves and carries the scope tag.
    auto sel = Selection::parse("test/(resi 10 and name CA)", mol);
    CHECK(sel.size() == 1, "test/(...) resolves against the matching object");
    CHECK(sel.objectScope() == "test", "test/(...) tags object scope");
}

static void testObjQualifierMismatch(const MolObject& mol) {
    // A qualifier naming a different object matches nothing here.
    auto sel = Selection::parse("other/(resi 10 and name CA)", mol);
    CHECK(sel.empty(), "other/(...) is empty against object 'test'");
}

static void testObjWildcardUnscoped(const MolObject& mol) {
    auto a = Selection::parse("all/(resi 10 and name CA)", mol);
    CHECK(a.size() == 1 && a.objectScope().empty(),
          "all/(...) matches and stays unscoped");
    auto b = Selection::parse("*/(resi 10 and name CA)", mol);
    CHECK(b.size() == 1 && b.objectScope().empty(),
          "*/(...) matches and stays unscoped");
}

static void testSlashFormScope(const MolObject& mol) {
    auto sel = Selection::parse("/test/A/10/CA", mol);
    CHECK(sel.size() == 1, "/test/A/10/CA resolves");
    CHECK(sel.objectScope() == "test", "/test/.../ tags object scope");
    auto glob = Selection::parse("/*/A/10/CA", mol);
    CHECK(glob.objectScope().empty(), "/*/.../ stays unscoped");
}

static void testNamedSelectionScopeGate(const MolObject& mol) {
    // A named selection scoped to "test" re-expands only against an object
    // of that name; against a differently-named object it matches nothing
    // (the core of issue #101 — no leak into other loaded objects).
    Selection scoped({0, 1, 2}, "test/(...)");
    scoped.setObjectScope("test");
    auto resolver = [&](const std::string& n) -> const Selection* {
        return (n == "a") ? &scoped : nullptr;
    };
    auto same = Selection::parse("$a", mol, resolver);
    CHECK(same.size() == 3, "$a re-expands against its own object");

    MolObject other("other");
    auto& oa = other.atoms();
    for (int i = 0; i < 5; ++i)
        oa.push_back({0,0,0, "CA", "C", "ALA", "A", 10 + i, ' ', 0, 1,
                      i + 1, 0, false, SSType::Loop});
    auto cross = Selection::parse("$a", other, resolver);
    CHECK(cross.empty(), "$a does not leak into a different object");
}

static void testUnscopedNamedSelectionStillResolves(const MolObject& mol) {
    // An unscoped named selection keeps the legacy behaviour (resolves
    // against whatever object it is parsed against).
    Selection plain({0, 1, 2}, "resi 10");
    auto resolver = [&](const std::string& n) -> const Selection* {
        return (n == "p") ? &plain : nullptr;
    };
    MolObject other("other");
    auto& oa = other.atoms();
    for (int i = 0; i < 5; ++i)
        oa.push_back({0,0,0, "CA", "C", "ALA", "A", 10 + i, ' ', 0, 1,
                      i + 1, 0, false, SSType::Loop});
    auto sel = Selection::parse("$p", other, resolver);
    CHECK(sel.size() == 3, "unscoped $p still resolves against other object");
}

static void testScopedOrKeepsScope(const MolObject& mol) {
    auto sel = Selection::parse(
        "test/(resi 10 and name CA) or test/(resi 11 and name CA)", mol);
    CHECK(sel.size() == 2, "scoped OR unions both residues");
    CHECK(sel.objectScope() == "test", "OR of same-scope terms keeps scope");
}

// ── Complex combos ───────────────────────────────────────────────────────────

static void testChainAndResiAndName(const MolObject& mol) {
    auto sel = Selection::parse("chain A and resi 10 and name CA", mol);
    auto s = toSet(sel);
    CHECK(s.count(1), "A:10:CA found");
    CHECK(sel.size() == 1, "single atom selected");
}

static void testOrChains(const MolObject& mol) {
    auto sel = Selection::parse("(chain A or chain B or chain H) and name N", mol);
    std::set<int> expected = {0, 4, 8, 12, 14, 16};
    CHECK(toSet(sel) == expected, "all N atoms across A, B, H");
}

static void testComplexNesting(const MolObject& mol) {
    // (chain H and resi 26-32) or (chain A and resn GLY)
    auto sel = Selection::parse("(chain H and resi 26-32) or (chain A and resn GLY)", mol);
    auto s = toSet(sel);
    CHECK(s.count(12), "H:26 in result");
    CHECK(s.count(14), "H:30 in result");
    CHECK(s.count(4), "A:GLY in result");
    CHECK(!s.count(0), "A:ALA not in result");
    CHECK(!s.count(16), "H:35 not in result");
}

static void testWaterOrHydro(const MolObject& mol) {
    auto sel = Selection::parse("water or hydro", mol);
    auto s = toSet(sel);
    CHECK(s.count(18), "water or hydro: HOH");
    CHECK(s.count(19), "water or hydro: WAT");
    CHECK(s.count(20), "water or hydro: H");
    CHECK(s.count(21), "water or hydro: DOD");
    CHECK(s.count(45), "water or hydro: nucleotide base H");
    CHECK(sel.size() == 5, "water or hydro = 5 (3 waters + 2 H)");
}

static void testNotWaterAndNotHydro(const MolObject& mol) {
    auto all = Selection::parse("all", mol);
    auto sel = Selection::parse("not (water or hydro)", mol);
    auto waterHydro = Selection::parse("water or hydro", mol);
    CHECK(sel.size() + waterHydro.size() == all.size(), "complement works");
}

static void testEmpty(const MolObject& mol) {
    auto sel = Selection::parse("", mol);
    CHECK(sel.empty(), "empty string → empty selection");
}

// Nucleic-acid substructure selectors (issue #111). The DG nucleotide is at
// indices 23-45: phosphate {23-27}, sugar {28-33}, base heavy {34-44}, H {45}.
static void testNucleicSubstructure(const MolObject& mol) {
    auto phos = toSet(Selection::parse("phosphate", mol));
    CHECK(phos.size() == 5, "phosphate = 5 atoms");
    CHECK(phos.count(23) && phos.count(26), "phosphate includes P and O5'");
    CHECK(!phos.count(28) && !phos.count(34), "phosphate excludes sugar/base");
    CHECK(!phos.count(0) && !phos.count(22), "phosphate ignores protein/ligand");

    auto sugar = toSet(Selection::parse("sugar", mol));
    CHECK(sugar.size() == 6, "sugar = 6 atoms");
    CHECK(sugar.count(28) && sugar.count(30), "sugar includes C5' and O4'");
    CHECK(!sugar.count(26) && !sugar.count(23), "sugar excludes O5'/P (phosphate)");

    auto base = toSet(Selection::parse("base", mol));
    CHECK(base.size() == 11, "base = 11 heavy ring atoms");
    CHECK(base.count(34) && base.count(38), "base includes N9 and C6");
    CHECK(!base.count(45), "base excludes the base hydrogen");
    CHECK(!base.count(23) && !base.count(28), "base excludes phosphate/sugar");

    // backbone is nucleic-aware: phosphate + sugar for a nucleotide.
    auto bbD = toSet(Selection::parse("chain D and backbone", mol));
    CHECK(bbD.size() == 11, "nucleic backbone = phosphate + sugar (11)");
    CHECK(bbD.count(23) && bbD.count(28), "nucleic backbone has P and C5'");
    CHECK(!bbD.count(34), "nucleic backbone excludes base");

    // backbone still resolves protein N/CA/C/O for amino acids.
    auto bbA = toSet(Selection::parse("chain A and backbone", mol));
    CHECK(bbA.count(0) && bbA.count(1) && bbA.count(2) && bbA.count(3),
          "protein backbone still N/CA/C/O");

    // sidechain is the per-residue complement: the base (+ its H) for a
    // nucleotide.
    auto scD = toSet(Selection::parse("chain D and sidechain", mol));
    CHECK(scD.size() == 12, "nucleic sidechain = base heavy + H (12)");
    CHECK(scD.count(34) && scD.count(45), "nucleic sidechain includes base + its H");
    CHECK(!scD.count(23) && !scD.count(28), "nucleic sidechain excludes backbone");

    // Clean partition: phosphate ∪ sugar ∪ base == all heavy nucleotide atoms.
    auto nuc = toSet(Selection::parse("chain D", mol));
    CHECK(nuc.size() == 23, "DG nucleotide has 23 atoms (incl. 1 H)");
    CHECK(phos.size() + sugar.size() + base.size() == nuc.size() - 1,
          "phosphate+sugar+base partitions the nucleotide heavy atoms");
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main() {
    auto mol = makeMol();

    std::cout << "Running selection parser tests...\n\n";

    // Chain tests
    testChainBasic(mol);
    testChainMultiple(mol);
    testChainH(mol);

    // Resi tests
    testResiSingle(mol);
    testResiRange(mol);
    testResiRangeChainH(mol);  // THE crash case
    testResiMultiRange(mol);
    testResiMixedRange(mol);

    // Resn / name / element
    testResn(mol);
    testName(mol);
    testNameMultiple(mol);
    testElement(mol);

    // Boolean algebra
    testAndOperator(mol);
    testOrOperator(mol);
    testNotOperator(mol);
    testParentheses(mol);
    testNestedBoolean(mol);
    testNotWithAnd(mol);
    testDoubleNot(mol);

    // Water / Hydro
    testWater(mol);
    testHOH(mol);
    testSol(mol);
    testHydro(mol);
    testNotWater(mol);
    testProteinAndNotWater(mol);

    // SS types
    testHelix(mol);
    testSheet(mol);
    testLoop(mol);

    // Backbone / Sidechain
    testBackbone(mol);
    testBB(mol);
    testSidechain(mol);

    // Het / Protein
    testHet(mol);
    testProtein(mol);

    // Slash notation
    testSlashChainResi(mol);
    testSlashChainResiRange(mol);
    testSlashName(mol);

    // All / Empty
    testAll(mol);
    testEmpty(mol);

    // Named selections
    testNamedSelection(mol);
    testNamedAndChain(mol);

    // Object qualifier scope (issue #101)
    testObjQualifierMatch(mol);
    testObjQualifierMismatch(mol);
    testObjWildcardUnscoped(mol);
    testSlashFormScope(mol);
    testNamedSelectionScopeGate(mol);
    testUnscopedNamedSelectionStillResolves(mol);
    testScopedOrKeepsScope(mol);

    // Complex combos
    testChainAndResiAndName(mol);
    testOrChains(mol);
    testComplexNesting(mol);
    testWaterOrHydro(mol);
    testNotWaterAndNotHydro(mol);

    // Nucleic-acid substructure selectors (issue #111)
    testNucleicSubstructure(mol);

    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed > 0 ? 1 : 0;
}
