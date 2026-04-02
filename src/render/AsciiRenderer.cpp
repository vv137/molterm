#include "molterm/render/AsciiRenderer.h"
#include <cmath>
#include <algorithm>

namespace molterm {

void AsciiRenderer::render(const MolObject& mol, const Camera& cam,
                           DepthBuffer& zbuf, Window& win) {
    if (!mol.visible()) return;

    // Render bonds first (behind atoms)
    if (mol.reprVisible(ReprType::Wireframe)) {
        renderBonds(mol, cam, zbuf, win);
    }
    renderAtoms(mol, cam, zbuf, win);
}

void AsciiRenderer::renderAtoms(const MolObject& mol, const Camera& cam,
                                DepthBuffer& zbuf, Window& win) {
    int w = win.width(), h = win.height();
    const auto& atoms = mol.atoms();

    for (const auto& atom : atoms) {
        int sx, sy;
        float depth;
        if (!cam.project(atom.x, atom.y, atom.z, w, h, sx, sy, depth))
            continue;

        if (sx < 0 || sx >= w || sy < 0 || sy >= h) continue;

        if (zbuf.testAndSet(sx, sy, depth)) {
            char ch = atomChar(atom);
            int color = ColorMapper::colorForAtom(atom, mol.colorScheme());
            win.addCharColored(sy, sx, ch, color);
        }
    }
}

void AsciiRenderer::renderBonds(const MolObject& mol, const Camera& cam,
                                DepthBuffer& zbuf, Window& win) {
    int w = win.width(), h = win.height();
    const auto& atoms = mol.atoms();
    const auto& bonds = mol.bonds();

    for (const auto& bond : bonds) {
        if (bond.atom1 < 0 || bond.atom1 >= static_cast<int>(atoms.size())) continue;
        if (bond.atom2 < 0 || bond.atom2 >= static_cast<int>(atoms.size())) continue;

        const auto& a1 = atoms[bond.atom1];
        const auto& a2 = atoms[bond.atom2];

        int sx1, sy1, sx2, sy2;
        float d1, d2;
        if (!cam.project(a1.x, a1.y, a1.z, w, h, sx1, sy1, d1)) continue;
        if (!cam.project(a2.x, a2.y, a2.z, w, h, sx2, sy2, d2)) continue;

        // Skip if both endpoints off screen
        if ((sx1 < 0 && sx2 < 0) || (sx1 >= w && sx2 >= w)) continue;
        if ((sy1 < 0 && sy2 < 0) || (sy1 >= h && sy2 >= h)) continue;

        int color = ColorMapper::colorForAtom(a1, mol.colorScheme());
        drawLine(sx1, sy1, d1, sx2, sy2, d2, '.', color, zbuf, win);
    }
}

void AsciiRenderer::drawLine(int x0, int y0, float d0,
                              int x1, int y1, float d1,
                              char ch, int colorPair,
                              DepthBuffer& zbuf, Window& win) {
    int dx = std::abs(x1 - x0);
    int dy = std::abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;
    int steps = std::max(dx, dy);
    if (steps == 0) steps = 1;

    int w = win.width(), h = win.height();
    int step = 0;

    while (true) {
        float t = static_cast<float>(step) / static_cast<float>(steps);
        float depth = d0 + (d1 - d0) * t;

        if (x0 >= 0 && x0 < w && y0 >= 0 && y0 < h) {
            // Choose line character based on direction
            char lineChar = ch;
            if (dx > dy * 2) lineChar = '-';
            else if (dy > dx * 2) lineChar = '|';
            else lineChar = (sx == sy) ? '\\' : '/';

            if (zbuf.testAndSet(x0, y0, depth)) {
                win.addCharColored(y0, x0, lineChar, colorPair);
            }
        }

        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
        ++step;
    }
}

char AsciiRenderer::atomChar(const AtomData& atom) const {
    if (atom.element.size() == 1) return atom.element[0];
    if (atom.element.size() >= 2) return atom.element[0];
    return '*';
}

} // namespace molterm
