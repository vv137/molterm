#pragma once

namespace molterm {

struct BondData {
    int atom1 = 0;
    int atom2 = 0;
    int order = 1;  // 1=single, 2=double, 3=triple
};

} // namespace molterm
