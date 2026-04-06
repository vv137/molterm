// tests/test_selection.cpp — Selection parser unit tests
// Build:  g++ -std=c++17 -I include tests/test_selection.cpp src/core/Selection.cpp src/core/MolObject.cpp -o test_selection
// Run:    ./test_selection

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
    CHECK(sel.size() == 1, "hydro = 1 hydrogen atom");
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
    CHECK(sel.size() == 4, "water or hydro = 4");
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

    // Complex combos
    testChainAndResiAndName(mol);
    testOrChains(mol);
    testComplexNesting(mol);
    testWaterOrHydro(mol);
    testNotWaterAndNotHydro(mol);

    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed > 0 ? 1 : 0;
}
