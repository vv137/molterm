#pragma once

namespace molterm {

// Side-by-side stereoscopic rendering. Walleye = parallel viewing
// (left image to left eye); Crosseye = the user crosses their eyes
// (left image to right eye).
enum class StereoMode { Off, Walleye, Crosseye };

} // namespace molterm
