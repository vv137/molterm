#pragma once

#include "molterm/core/MolObject.h"
#include "molterm/render/Camera.h"
#include "molterm/render/Canvas.h"

namespace molterm {

// Abstract representation: knows what to draw from a MolObject.
// Uses Canvas for backend-agnostic sub-pixel drawing.
class Representation {
public:
    virtual ~Representation() = default;

    virtual ReprType type() const = 0;

    // Render this representation of mol onto canvas
    virtual void render(const MolObject& mol, const Camera& cam,
                        Canvas& canvas) = 0;
};

} // namespace molterm
