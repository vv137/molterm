#pragma once

#include <functional>
#include <vector>

#include "molterm/tui/Window.h"

namespace molterm {

// Reusable half-block heatmap renderer.
// Renders a 2D float matrix as a colored grid using half-block characters (▀▄█).
// Each terminal cell encodes 2 vertical data pixels (top/bottom halves).
class DensityMap {
public:
    // Set data: row-major float matrix, values in [0,1].
    void setData(const std::vector<float>& data, int rows, int cols);
    void setData(std::vector<float>&& data, int rows, int cols);

    // Color ramp: maps [0,1] → ncurses color pair.
    // Default: blue(cold) → gray → red(hot), 5-step heatmap.
    using ColorFunc = std::function<int(float)>;
    void setColorFunc(ColorFunc fn);

    // Render into window at given scroll offset.
    // winRowOffset: start rendering at this window row (e.g., 1 to skip a header line).
    void render(Window& win, int scrollRow = 0, int scrollCol = 0, int winRowOffset = 0);

    // Map terminal cell (ty, tx) back to matrix (row, col).
    // Returns false if out of bounds.
    bool cellToMatrix(int ty, int tx, int scrollRow, int scrollCol,
                      int& outRow, int& outCol) const;

    int dataRows() const { return rows_; }
    int dataCols() const { return cols_; }

    // Default 5-step heatmap color function
    static int defaultColorFunc(float v);

private:
    std::vector<float> data_;
    int rows_ = 0, cols_ = 0;
    ColorFunc colorFunc_;
};

} // namespace molterm
