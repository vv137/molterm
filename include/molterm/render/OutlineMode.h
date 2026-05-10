#pragma once

namespace molterm {

// Outline post-pass behavior shared by PixelCanvas::applyOutline and
// Application's :set outline_mode handler.
//   Edge       : darken silhouette pixels (legacy default; works on light bg).
//   Silhouette : paint silhouette pixels a fixed color regardless of underlying
//                pixel — closes the dark-bg gap where "darken" of black is
//                still black (issue #31).
//   Both       : silhouette paint THEN edge darken — colored rim with subtle
//                interior depth-edge darkening on top.
enum class OutlineMode { Edge, Silhouette, Both };

}  // namespace molterm
