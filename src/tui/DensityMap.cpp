#include "molterm/tui/DensityMap.h"
#include "molterm/render/ColorMapper.h"

namespace molterm {

static constexpr char32_t FULL_BLOCK = 0x2588;
static constexpr char32_t UPPER_HALF = 0x2580;
static constexpr char32_t LOWER_HALF = 0x2584;

void DensityMap::setData(const std::vector<float>& data, int rows, int cols) {
    data_ = data;
    rows_ = rows;
    cols_ = cols;
}

void DensityMap::setData(std::vector<float>&& data, int rows, int cols) {
    data_ = std::move(data);
    rows_ = rows;
    cols_ = cols;
}

void DensityMap::setColorFunc(ColorFunc fn) {
    colorFunc_ = std::move(fn);
}

int DensityMap::defaultColorFunc(float v) {
    if (v < 0.2f) return kColorHeatmap0;
    if (v < 0.4f) return kColorHeatmap1;
    if (v < 0.6f) return kColorHeatmap2;
    if (v < 0.8f) return kColorHeatmap3;
    return kColorHeatmap4;
}

void DensityMap::render(Window& win, int scrollRow, int scrollCol, int winRowOffset) {
    int winH = win.height() - winRowOffset;
    int winW = win.width();

    auto colorFn = colorFunc_ ? colorFunc_ : defaultColorFunc;

    // Each terminal row covers 2 data rows (top/bottom half-block)
    for (int ty = 0; ty < winH; ++ty) {
        int r_top = scrollRow + ty * 2;
        int r_bot = scrollRow + ty * 2 + 1;

        for (int tx = 0; tx < winW; ++tx) {
            int col = scrollCol + tx;

            bool topValid = (r_top >= 0 && r_top < rows_ && col >= 0 && col < cols_);
            bool botValid = (r_bot >= 0 && r_bot < rows_ && col >= 0 && col < cols_);

            float vTop = topValid ? data_[static_cast<size_t>(r_top) * cols_ + col] : -1.0f;
            float vBot = botValid ? data_[static_cast<size_t>(r_bot) * cols_ + col] : -1.0f;

            int cTop = topValid ? colorFn(vTop) : 0;
            int cBot = botValid ? colorFn(vBot) : 0;

            int wy = ty + winRowOffset;
            if (!topValid && !botValid) {
                win.addChar(wy, tx, ' ');
            } else if (topValid && botValid) {
                if (cTop == cBot) {
                    win.addWideChar(wy, tx, FULL_BLOCK, cTop);
                } else {
                    // Two-color half-blocks require per-cell bg — not available.
                    // Use the dominant (hotter) color for the full cell.
                    win.addWideChar(wy, tx, FULL_BLOCK, (vTop >= vBot) ? cTop : cBot);
                }
            } else if (topValid) {
                win.addWideChar(wy, tx, UPPER_HALF, cTop);
            } else {
                win.addWideChar(wy, tx, LOWER_HALF, cBot);
            }
        }
    }
}

bool DensityMap::cellToMatrix(int ty, int tx, int scrollRow, int scrollCol,
                              int& outRow, int& outCol) const {
    // Return the top data row for this cell
    outRow = scrollRow + ty * 2;
    outCol = scrollCol + tx;
    return (outRow >= 0 && outRow < rows_ && outCol >= 0 && outCol < cols_);
}

} // namespace molterm
