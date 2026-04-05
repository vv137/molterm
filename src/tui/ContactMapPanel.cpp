#include "molterm/tui/ContactMapPanel.h"
#include "molterm/render/ColorMapper.h"

#include <algorithm>
#include <cmath>

namespace molterm {

void ContactMapPanel::update(const MolObject& mol, float cutoff) {
    if (mol.name() == lastObjName_ &&
        static_cast<int>(mol.atoms().size()) == lastAtomCount_ &&
        cutoff == lastCutoff_ && contactMap_.valid()) {
        return;
    }

    lastObjName_ = mol.name();
    lastAtomCount_ = static_cast<int>(mol.atoms().size());
    lastCutoff_ = cutoff;
    scrollRow_ = 0;
    scrollCol_ = 0;

    contactMap_.compute(mol, cutoff);

    int n = contactMap_.residueCount();
    if (n == 0) return;

    std::vector<float> matrix(static_cast<size_t>(n) * n, 0.0f);
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            float d = contactMap_.distance(i, j);
            if (!std::isnan(d)) {
                matrix[static_cast<size_t>(i) * n + j] = 1.0f - std::min(d / cutoff, 1.0f);
            }
        }
    }

    densityMap_.setData(std::move(matrix), n, n);
}

void ContactMapPanel::render(Window& win) {
    win.erase();
    int w = win.width();
    int h = win.height();
    if (w < 3 || h < 2) return;

    if (!contactMap_.valid()) {
        win.printColored(0, 1, "No contact map", kColorPanelHeader);
        return;
    }

    // Header
    int n = contactMap_.residueCount();
    std::string header = "CMap " + std::to_string(n) + "r";
    if (static_cast<int>(header.size()) > w - 1)
        header = header.substr(0, w - 1);
    win.printColored(0, 1, header, kColorPanelHeader);

    // Clamp scroll
    int mapH = h - 1;
    int maxScrollRow = std::max(0, densityMap_.dataRows() - mapH * 2);
    int maxScrollCol = std::max(0, densityMap_.dataCols() - w);
    scrollRow_ = std::clamp(scrollRow_, 0, maxScrollRow);
    scrollCol_ = std::clamp(scrollCol_, 0, maxScrollCol);

    // Delegate rendering to DensityMap (row offset 1 for header)
    densityMap_.render(win, scrollRow_, scrollCol_, 1);

    // Axis ticks
    int rows = densityMap_.dataRows();
    for (int ty = 0; ty < mapH; ++ty) {
        int r = scrollRow_ + ty * 2;
        if (r < rows && r % 20 == 0) {
            const auto& res = contactMap_.residues();
            if (r < static_cast<int>(res.size())) {
                std::string tick = std::to_string(res[r].resSeq);
                if (static_cast<int>(tick.size()) <= w)
                    win.printColored(ty + 1, 0, tick, kColorWhite);
            }
        }
    }
}

std::pair<int,int> ContactMapPanel::residueAtCell(int ty, int tx) const {
    if (!contactMap_.valid()) return {-1, -1};

    int mapTy = ty - 1;  // account for header row
    if (mapTy < 0) return {-1, -1};

    int row = scrollRow_ + mapTy * 2;
    int col = scrollCol_ + tx;

    const auto& res = contactMap_.residues();
    int n = static_cast<int>(res.size());

    if (row < 0 || row >= n || col < 0 || col >= n)
        return {-1, -1};

    return {res[row].resSeq, res[col].resSeq};
}

} // namespace molterm
