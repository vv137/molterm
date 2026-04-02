#pragma once

#include "molterm/render/Renderer.h"
#include "molterm/render/ColorMapper.h"

namespace molterm {

class AsciiRenderer : public Renderer {
public:
    void render(const MolObject& mol, const Camera& cam,
                DepthBuffer& zbuf, Window& win) override;

private:
    void renderAtoms(const MolObject& mol, const Camera& cam,
                     DepthBuffer& zbuf, Window& win);
    void renderBonds(const MolObject& mol, const Camera& cam,
                     DepthBuffer& zbuf, Window& win);

    // Bresenham line drawing with depth interpolation
    void drawLine(int x0, int y0, float d0,
                  int x1, int y1, float d1,
                  char ch, int colorPair,
                  DepthBuffer& zbuf, Window& win);

    char atomChar(const AtomData& atom) const;
};

} // namespace molterm
