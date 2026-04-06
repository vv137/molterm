// tests/test_constraint.cpp — Constraint layout solver unit tests
// Build: g++ -std=c++17 -I include tests/test_constraint.cpp -o test_constraint
// Run:   ./test_constraint

#include <cassert>
#include <iostream>
#include <vector>

#include "molterm/tui/Constraint.h"

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

// ── solveStrip tests ─────────────────────────────────────────────────────────

static void testAllFixed() {
    std::vector<LayoutSlot> slots = {
        {SizePolicy::Fixed, 1, true, 0, 0},
        {SizePolicy::Fixed, 2, true, 0, 0},
        {SizePolicy::Fixed, 3, true, 0, 0},
    };
    auto sizes = solveStrip(10, slots);
    CHECK(sizes[0] == 1, "allFixed: slot 0 = 1");
    CHECK(sizes[1] == 2, "allFixed: slot 1 = 2");
    CHECK(sizes[2] == 3, "allFixed: slot 2 = 3");
}

static void testSingleFill() {
    std::vector<LayoutSlot> slots = {
        {SizePolicy::Fixed, 1, true, 0, 0},
        {SizePolicy::Fill,  1, true, 0, 0},
        {SizePolicy::Fixed, 1, true, 0, 0},
    };
    auto sizes = solveStrip(20, slots);
    CHECK(sizes[0] == 1, "singleFill: fixed 0 = 1");
    CHECK(sizes[1] == 18, "singleFill: fill = 18");
    CHECK(sizes[2] == 1, "singleFill: fixed 2 = 1");
}

static void testMultiFillWeighted() {
    std::vector<LayoutSlot> slots = {
        {SizePolicy::Fill, 1, true, 0, 0},
        {SizePolicy::Fill, 3, true, 0, 0},
    };
    auto sizes = solveStrip(100, slots);
    CHECK(sizes[0] == 25, "weighted: 1/4 = 25");
    CHECK(sizes[1] == 75, "weighted: 3/4 = 75");
}

static void testInvisibleSlot() {
    std::vector<LayoutSlot> slots = {
        {SizePolicy::Fixed, 5, true, 0, 0},
        {SizePolicy::Fixed, 5, false, 0, 0},  // invisible
        {SizePolicy::Fill,  1, true, 0, 0},
    };
    auto sizes = solveStrip(20, slots);
    CHECK(sizes[0] == 5, "invisible: visible fixed = 5");
    CHECK(sizes[1] == 0, "invisible: invisible = 0");
    CHECK(sizes[2] == 15, "invisible: fill gets rest = 15");
}

static void testMinSize() {
    std::vector<LayoutSlot> slots = {
        {SizePolicy::Fill, 1, true, 10, 0},  // min 10
        {SizePolicy::Fill, 1, true, 0, 0},
    };
    auto sizes = solveStrip(12, slots);
    CHECK(sizes[0] >= 10, "minSize: slot 0 >= 10");
}

static void testMaxSize() {
    std::vector<LayoutSlot> slots = {
        {SizePolicy::Fill, 1, true, 0, 5},   // max 5
        {SizePolicy::Fill, 1, true, 0, 0},
    };
    auto sizes = solveStrip(100, slots);
    CHECK(sizes[0] <= 5, "maxSize: slot 0 <= 5");
    CHECK(sizes[0] + sizes[1] == 100, "maxSize: total = 100");
}

// ── solveLayout tests ────────────────────────────────────────────────────────

static void testVerticalLayout() {
    Rect area = {0, 0, 40, 80};
    std::vector<LayoutSlot> slots = {
        {SizePolicy::Fixed, 1, true, 0, 0},   // TabBar
        {SizePolicy::Fill,  1, true, 1, 0},   // Viewport
        {SizePolicy::Fixed, 1, true, 0, 0},   // SeqBar
        {SizePolicy::Fixed, 1, true, 0, 0},   // StatusBar
        {SizePolicy::Fixed, 1, true, 0, 0},   // CommandLine
    };
    auto rects = solveLayout(area, SplitDir::Vertical, slots);

    CHECK(rects[0].y == 0 && rects[0].h == 1, "vLayout: tabbar row 0, h=1");
    CHECK(rects[1].y == 1, "vLayout: viewport starts at row 1");
    CHECK(rects[1].h == 36, "vLayout: viewport height 40-4=36");
    CHECK(rects[1].w == 80, "vLayout: viewport full width");
    CHECK(rects[2].y == 37, "vLayout: seqbar at 37");
    CHECK(rects[3].y == 38, "vLayout: statusbar at 38");
    CHECK(rects[4].y == 39, "vLayout: cmdline at 39");

    // All have full width
    for (int i = 0; i < 5; ++i) {
        CHECK(rects[i].w == 80, "vLayout: all full width");
    }
}

static void testHorizontalLayout() {
    Rect area = {1, 0, 36, 80};
    std::vector<LayoutSlot> slots = {
        {SizePolicy::Fill,  1, true, 1, 0},    // Viewport
        {SizePolicy::Fixed, 22, true, 0, 0},   // Panel
    };
    auto rects = solveLayout(area, SplitDir::Horizontal, slots);

    CHECK(rects[0].x == 0, "hLayout: viewport starts at x=0");
    CHECK(rects[0].w == 58, "hLayout: viewport width 80-22=58");
    CHECK(rects[0].h == 36, "hLayout: viewport full height");
    CHECK(rects[1].x == 58, "hLayout: panel at x=58");
    CHECK(rects[1].w == 22, "hLayout: panel width 22");
}

static void testInvisiblePanel() {
    Rect area = {1, 0, 36, 80};
    std::vector<LayoutSlot> slots = {
        {SizePolicy::Fill,  1, true, 1, 0},
        {SizePolicy::Fixed, 22, false, 0, 0},  // hidden panel
    };
    auto rects = solveLayout(area, SplitDir::Horizontal, slots);

    CHECK(rects[0].w == 80, "hiddenPanel: viewport gets full width");
    CHECK(rects[1].w == 0, "hiddenPanel: panel width = 0");
}

static void testNestedLayout() {
    // Simulate full molterm layout: vertical → horizontal → vertical
    Rect screen = {0, 0, 40, 100};

    // Vertical: TabBar(1) | Middle(fill) | SeqBar(2) | Status(1) | Cmd(1)
    auto vRects = solveLayout(screen, SplitDir::Vertical, {
        {SizePolicy::Fixed, 1, true, 0, 0},
        {SizePolicy::Fill,  1, true, 1, 0},
        {SizePolicy::Fixed, 2, true, 0, 0},
        {SizePolicy::Fixed, 1, true, 0, 0},
        {SizePolicy::Fixed, 1, true, 0, 0},
    });

    // Middle row horizontal: Viewport(fill) | Panel(22)
    auto hRects = solveLayout(vRects[1], SplitDir::Horizontal, {
        {SizePolicy::Fill,  1, true, 1, 0},
        {SizePolicy::Fixed, 22, true, 0, 0},
    });

    // Panel vertical: ObjPanel(6) | AnalysisPanel(fill)
    auto pRects = solveLayout(hRects[1], SplitDir::Vertical, {
        {SizePolicy::Fixed, 6, true, 0, 0},
        {SizePolicy::Fill,  1, true, 1, 0},
    });

    CHECK(vRects[0].h == 1, "nested: tabbar h=1");
    CHECK(vRects[1].h == 35, "nested: middle h=35");
    CHECK(vRects[2].h == 2, "nested: seqbar h=2");

    CHECK(hRects[0].w == 78, "nested: viewport w=78");
    CHECK(hRects[1].w == 22, "nested: panel w=22");

    CHECK(pRects[0].h == 6, "nested: objpanel h=6");
    CHECK(pRects[1].h == 29, "nested: analysis h=29");
    CHECK(pRects[0].x == hRects[1].x, "nested: objpanel aligned to panel x");
}

static void testSeqBarHidden() {
    Rect screen = {0, 0, 40, 80};
    auto rects = solveLayout(screen, SplitDir::Vertical, {
        {SizePolicy::Fixed, 1, true, 0, 0},
        {SizePolicy::Fill,  1, true, 1, 0},
        {SizePolicy::Fixed, 1, false, 0, 0},  // seqbar hidden
        {SizePolicy::Fixed, 1, true, 0, 0},
        {SizePolicy::Fixed, 1, true, 0, 0},
    });

    CHECK(rects[1].h == 37, "seqHidden: viewport gets extra row (40-3=37)");
    CHECK(rects[2].h == 0, "seqHidden: seqbar h=0");
    CHECK(rects[3].y == 38, "seqHidden: statusbar at 38");
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "Running constraint solver tests...\n\n";

    // Strip tests
    testAllFixed();
    testSingleFill();
    testMultiFillWeighted();
    testInvisibleSlot();
    testMinSize();
    testMaxSize();

    // Layout tests
    testVerticalLayout();
    testHorizontalLayout();
    testInvisiblePanel();
    testNestedLayout();
    testSeqBarHidden();

    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed > 0 ? 1 : 0;
}
