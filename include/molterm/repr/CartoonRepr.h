#pragma once

#include "molterm/repr/Representation.h"
#include "molterm/render/ColorMapper.h"

namespace molterm {

// Cartoon: secondary structure-aware backbone trace
// - Helices: thick trace
// - Sheets: wide arrows
// - Loops: thin trace
class CartoonRepr : public Representation {
public:
    ReprType type() const override { return ReprType::Cartoon; }

    void render(const MolObject& mol, const Camera& cam,
                Canvas& canvas) override;

    float helixWidth() const { return helixWidth_; }
    void setHelixWidth(float w) { helixWidth_ = w; }
    float sheetWidth() const { return sheetWidth_; }
    void setSheetWidth(float w) { sheetWidth_ = w; }
    float loopWidth() const { return loopWidth_; }
    void setLoopWidth(float w) { loopWidth_ = w; }

private:
    float helixWidth_ = 3.0f;   // sub-pixel thickness
    float sheetWidth_ = 3.5f;
    float loopWidth_ = 1.0f;
};

} // namespace molterm
