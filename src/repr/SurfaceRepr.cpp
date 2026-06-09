#include "molterm/repr/SurfaceRepr.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "molterm/core/VdwTable.h"
#include "molterm/repr/ReprUtil.h"

namespace molterm {

namespace {

// ---- Marching cubes lookup tables (Paul Bourke convention) -----------------
// Corner numbering for a cube at grid cell (i,j,k):
//   0:(i,j,k)   1:(i+1,j,k)   2:(i+1,j,k+1)   3:(i,j,k+1)
//   4:(i,j+1,k) 5:(i+1,j+1,k) 6:(i+1,j+1,k+1) 7:(i,j+1,k+1)
// Edge e connects corners (edgeCorners[e][0], edgeCorners[e][1]).
constexpr int kEdgeCorners[12][2] = {
    {0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
    {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7},
};

// edgeTable[cubeIndex] = 12-bit mask of edges crossed by the surface.
const int kEdgeTable[256] = {
    0x0,   0x109, 0x203, 0x30a, 0x406, 0x50f, 0x605, 0x70c,
    0x80c, 0x905, 0xa0f, 0xb06, 0xc0a, 0xd03, 0xe09, 0xf00,
    0x190, 0x99,  0x393, 0x29a, 0x596, 0x49f, 0x795, 0x69c,
    0x99c, 0x895, 0xb9f, 0xa96, 0xd9a, 0xc93, 0xf99, 0xe90,
    0x230, 0x339, 0x33,  0x13a, 0x636, 0x73f, 0x435, 0x53c,
    0xa3c, 0xb35, 0x83f, 0x936, 0xe3a, 0xf33, 0xc39, 0xd30,
    0x3a0, 0x2a9, 0x1a3, 0xaa,  0x7a6, 0x6af, 0x5a5, 0x4ac,
    0xbac, 0xaa5, 0x9af, 0x8a6, 0xfaa, 0xea3, 0xda9, 0xca0,
    0x460, 0x569, 0x663, 0x76a, 0x66,  0x16f, 0x265, 0x36c,
    0xc6c, 0xd65, 0xe6f, 0xf66, 0x86a, 0x963, 0xa69, 0xb60,
    0x5f0, 0x4f9, 0x7f3, 0x6fa, 0x1f6, 0xff,  0x3f5, 0x2fc,
    0xdfc, 0xcf5, 0xfff, 0xef6, 0x9fa, 0x8f3, 0xbf9, 0xaf0,
    0x650, 0x759, 0x453, 0x55a, 0x256, 0x35f, 0x55,  0x15c,
    0xe5c, 0xf55, 0xc5f, 0xd56, 0xa5a, 0xb53, 0x859, 0x950,
    0x7c0, 0x6c9, 0x5c3, 0x4ca, 0x3c6, 0x2cf, 0x1c5, 0xcc,
    0xfcc, 0xec5, 0xdcf, 0xcc6, 0xbca, 0xac3, 0x9c9, 0x8c0,
    0x8c0, 0x9c9, 0xac3, 0xbca, 0xcc6, 0xdcf, 0xec5, 0xfcc,
    0xcc,  0x1c5, 0x2cf, 0x3c6, 0x4ca, 0x5c3, 0x6c9, 0x7c0,
    0x950, 0x859, 0xb53, 0xa5a, 0xd56, 0xc5f, 0xf55, 0xe5c,
    0x15c, 0x55,  0x35f, 0x256, 0x55a, 0x453, 0x759, 0x650,
    0xaf0, 0xbf9, 0x8f3, 0x9fa, 0xef6, 0xfff, 0xcf5, 0xdfc,
    0x2fc, 0x3f5, 0xff,  0x1f6, 0x6fa, 0x7f3, 0x4f9, 0x5f0,
    0xb60, 0xa69, 0x963, 0x86a, 0xf66, 0xe6f, 0xd65, 0xc6c,
    0x36c, 0x265, 0x16f, 0x66,  0x76a, 0x663, 0x569, 0x460,
    0xca0, 0xda9, 0xea3, 0xfaa, 0x8a6, 0x9af, 0xaa5, 0xbac,
    0x4ac, 0x5a5, 0x6af, 0x7a6, 0xaa,  0x1a3, 0x2a9, 0x3a0,
    0xd30, 0xc39, 0xf33, 0xe3a, 0x936, 0x83f, 0xb35, 0xa3c,
    0x53c, 0x435, 0x73f, 0x636, 0x13a, 0x33,  0x339, 0x230,
    0xe90, 0xf99, 0xc93, 0xd9a, 0xa96, 0xb9f, 0x895, 0x99c,
    0x69c, 0x795, 0x49f, 0x596, 0x29a, 0x393, 0x99,  0x190,
    0xf00, 0xe09, 0xd03, 0xc0a, 0xb06, 0xa0f, 0x905, 0x80c,
    0x70c, 0x605, 0x50f, 0x406, 0x30a, 0x203, 0x109, 0x0,
};

// triTable[cubeIndex] = up to 5 triangles as edge triples, -1 terminated.
const int kTriTable[256][16] = {
    {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {0,8,3,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {0,1,9,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {1,8,3,9,8,1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {1,2,10,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {0,8,3,1,2,10,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {9,2,10,0,2,9,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {2,8,3,2,10,8,10,9,8,-1,-1,-1,-1,-1,-1,-1},
    {3,11,2,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {0,11,2,8,11,0,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {1,9,0,2,3,11,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {1,11,2,1,9,11,9,8,11,-1,-1,-1,-1,-1,-1,-1},
    {3,10,1,11,10,3,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {0,10,1,0,8,10,8,11,10,-1,-1,-1,-1,-1,-1,-1},
    {3,9,0,3,11,9,11,10,9,-1,-1,-1,-1,-1,-1,-1},
    {9,8,10,10,8,11,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {4,7,8,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {4,3,0,7,3,4,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {0,1,9,8,4,7,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {4,1,9,4,7,1,7,3,1,-1,-1,-1,-1,-1,-1,-1},
    {1,2,10,8,4,7,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {3,4,7,3,0,4,1,2,10,-1,-1,-1,-1,-1,-1,-1},
    {9,2,10,9,0,2,8,4,7,-1,-1,-1,-1,-1,-1,-1},
    {2,10,9,2,9,7,2,7,3,7,9,4,-1,-1,-1,-1},
    {8,4,7,3,11,2,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {11,4,7,11,2,4,2,0,4,-1,-1,-1,-1,-1,-1,-1},
    {9,0,1,8,4,7,2,3,11,-1,-1,-1,-1,-1,-1,-1},
    {4,7,11,9,4,11,9,11,2,9,2,1,-1,-1,-1,-1},
    {3,10,1,3,11,10,7,8,4,-1,-1,-1,-1,-1,-1,-1},
    {1,11,10,1,4,11,1,0,4,7,11,4,-1,-1,-1,-1},
    {4,7,8,9,0,11,9,11,10,11,0,3,-1,-1,-1,-1},
    {4,7,11,4,11,9,9,11,10,-1,-1,-1,-1,-1,-1,-1},
    {9,5,4,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {9,5,4,0,8,3,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {0,5,4,1,5,0,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {8,5,4,8,3,5,3,1,5,-1,-1,-1,-1,-1,-1,-1},
    {1,2,10,9,5,4,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {3,0,8,1,2,10,4,9,5,-1,-1,-1,-1,-1,-1,-1},
    {5,2,10,5,4,2,4,0,2,-1,-1,-1,-1,-1,-1,-1},
    {2,10,5,3,2,5,3,5,4,3,4,8,-1,-1,-1,-1},
    {9,5,4,2,3,11,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {0,11,2,0,8,11,4,9,5,-1,-1,-1,-1,-1,-1,-1},
    {0,5,4,0,1,5,2,3,11,-1,-1,-1,-1,-1,-1,-1},
    {2,1,5,2,5,8,2,8,11,4,8,5,-1,-1,-1,-1},
    {10,3,11,10,1,3,9,5,4,-1,-1,-1,-1,-1,-1,-1},
    {4,9,5,0,8,1,8,10,1,8,11,10,-1,-1,-1,-1},
    {5,4,0,5,0,11,5,11,10,11,0,3,-1,-1,-1,-1},
    {5,4,8,5,8,10,10,8,11,-1,-1,-1,-1,-1,-1,-1},
    {9,7,8,5,7,9,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {9,3,0,9,5,3,5,7,3,-1,-1,-1,-1,-1,-1,-1},
    {0,7,8,0,1,7,1,5,7,-1,-1,-1,-1,-1,-1,-1},
    {1,5,3,3,5,7,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {9,7,8,9,5,7,10,1,2,-1,-1,-1,-1,-1,-1,-1},
    {10,1,2,9,5,0,5,3,0,5,7,3,-1,-1,-1,-1},
    {8,0,2,8,2,5,8,5,7,10,5,2,-1,-1,-1,-1},
    {2,10,5,2,5,3,3,5,7,-1,-1,-1,-1,-1,-1,-1},
    {7,9,5,7,8,9,3,11,2,-1,-1,-1,-1,-1,-1,-1},
    {9,5,7,9,7,2,9,2,0,2,7,11,-1,-1,-1,-1},
    {2,3,11,0,1,8,1,7,8,1,5,7,-1,-1,-1,-1},
    {11,2,1,11,1,7,7,1,5,-1,-1,-1,-1,-1,-1,-1},
    {9,5,8,8,5,7,10,1,3,10,3,11,-1,-1,-1,-1},
    {5,7,0,5,0,9,7,11,0,1,0,10,11,10,0,-1},
    {11,10,0,11,0,3,10,5,0,8,0,7,5,7,0,-1},
    {11,10,5,7,11,5,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {10,6,5,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {0,8,3,5,10,6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {9,0,1,5,10,6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {1,8,3,1,9,8,5,10,6,-1,-1,-1,-1,-1,-1,-1},
    {1,6,5,2,6,1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {1,6,5,1,2,6,3,0,8,-1,-1,-1,-1,-1,-1,-1},
    {9,6,5,9,0,6,0,2,6,-1,-1,-1,-1,-1,-1,-1},
    {5,9,8,5,8,2,5,2,6,3,2,8,-1,-1,-1,-1},
    {2,3,11,10,6,5,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {11,0,8,11,2,0,10,6,5,-1,-1,-1,-1,-1,-1,-1},
    {0,1,9,2,3,11,5,10,6,-1,-1,-1,-1,-1,-1,-1},
    {5,10,6,1,9,2,9,11,2,9,8,11,-1,-1,-1,-1},
    {6,3,11,6,5,3,5,1,3,-1,-1,-1,-1,-1,-1,-1},
    {0,8,11,0,11,5,0,5,1,5,11,6,-1,-1,-1,-1},
    {3,11,6,0,3,6,0,6,5,0,5,9,-1,-1,-1,-1},
    {6,5,9,6,9,11,11,9,8,-1,-1,-1,-1,-1,-1,-1},
    {5,10,6,4,7,8,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {4,3,0,4,7,3,6,5,10,-1,-1,-1,-1,-1,-1,-1},
    {1,9,0,5,10,6,8,4,7,-1,-1,-1,-1,-1,-1,-1},
    {10,6,5,1,9,7,1,7,3,7,9,4,-1,-1,-1,-1},
    {6,1,2,6,5,1,4,7,8,-1,-1,-1,-1,-1,-1,-1},
    {1,2,5,5,2,6,3,0,4,3,4,7,-1,-1,-1,-1},
    {8,4,7,9,0,5,0,6,5,0,2,6,-1,-1,-1,-1},
    {7,3,9,7,9,4,3,2,9,5,9,6,2,6,9,-1},
    {3,11,2,7,8,4,10,6,5,-1,-1,-1,-1,-1,-1,-1},
    {5,10,6,4,7,2,4,2,0,2,7,11,-1,-1,-1,-1},
    {0,1,9,4,7,8,2,3,11,5,10,6,-1,-1,-1,-1},
    {9,2,1,9,11,2,9,4,11,7,11,4,5,10,6,-1},
    {8,4,7,3,11,5,3,5,1,5,11,6,-1,-1,-1,-1},
    {5,1,11,5,11,6,1,0,11,7,11,4,0,4,11,-1},
    {0,5,9,0,6,5,0,3,6,11,6,3,8,4,7,-1},
    {6,5,9,6,9,11,4,7,9,7,11,9,-1,-1,-1,-1},
    {10,4,9,6,4,10,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {4,10,6,4,9,10,0,8,3,-1,-1,-1,-1,-1,-1,-1},
    {10,0,1,10,6,0,6,4,0,-1,-1,-1,-1,-1,-1,-1},
    {8,3,1,8,1,6,8,6,4,6,1,10,-1,-1,-1,-1},
    {1,4,9,1,2,4,2,6,4,-1,-1,-1,-1,-1,-1,-1},
    {3,0,8,1,2,9,2,4,9,2,6,4,-1,-1,-1,-1},
    {0,2,4,4,2,6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {8,3,2,8,2,4,4,2,6,-1,-1,-1,-1,-1,-1,-1},
    {10,4,9,10,6,4,11,2,3,-1,-1,-1,-1,-1,-1,-1},
    {0,8,2,2,8,11,4,9,10,4,10,6,-1,-1,-1,-1},
    {3,11,2,0,1,6,0,6,4,6,1,10,-1,-1,-1,-1},
    {6,4,1,6,1,10,4,8,1,2,1,11,8,11,1,-1},
    {9,6,4,9,3,6,9,1,3,11,6,3,-1,-1,-1,-1},
    {8,11,1,8,1,0,11,6,1,9,1,4,6,4,1,-1},
    {3,11,6,3,6,0,0,6,4,-1,-1,-1,-1,-1,-1,-1},
    {6,4,8,11,6,8,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {7,10,6,7,8,10,8,9,10,-1,-1,-1,-1,-1,-1,-1},
    {0,7,3,0,10,7,0,9,10,6,7,10,-1,-1,-1,-1},
    {10,6,7,1,10,7,1,7,8,1,8,0,-1,-1,-1,-1},
    {10,6,7,10,7,1,1,7,3,-1,-1,-1,-1,-1,-1,-1},
    {1,2,6,1,6,8,1,8,9,8,6,7,-1,-1,-1,-1},
    {2,6,9,2,9,1,6,7,9,0,9,3,7,3,9,-1},
    {7,8,0,7,0,6,6,0,2,-1,-1,-1,-1,-1,-1,-1},
    {7,3,2,6,7,2,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {2,3,11,10,6,8,10,8,9,8,6,7,-1,-1,-1,-1},
    {2,0,7,2,7,11,0,9,7,6,7,10,9,10,7,-1},
    {1,8,0,1,7,8,1,10,7,6,7,10,2,3,11,-1},
    {11,2,1,11,1,7,10,6,1,6,7,1,-1,-1,-1,-1},
    {8,9,6,8,6,7,9,1,6,11,6,3,1,3,6,-1},
    {0,9,1,11,6,7,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {7,8,0,7,0,6,3,11,0,11,6,0,-1,-1,-1,-1},
    {7,11,6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {7,6,11,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {3,0,8,11,7,6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {0,1,9,11,7,6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {8,1,9,8,3,1,11,7,6,-1,-1,-1,-1,-1,-1,-1},
    {10,1,2,6,11,7,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {1,2,10,3,0,8,6,11,7,-1,-1,-1,-1,-1,-1,-1},
    {2,9,0,2,10,9,6,11,7,-1,-1,-1,-1,-1,-1,-1},
    {6,11,7,2,10,3,10,8,3,10,9,8,-1,-1,-1,-1},
    {7,2,3,6,2,7,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {7,0,8,7,6,0,6,2,0,-1,-1,-1,-1,-1,-1,-1},
    {2,7,6,2,3,7,0,1,9,-1,-1,-1,-1,-1,-1,-1},
    {1,6,2,1,8,6,1,9,8,8,7,6,-1,-1,-1,-1},
    {10,7,6,10,1,7,1,3,7,-1,-1,-1,-1,-1,-1,-1},
    {10,7,6,1,7,10,1,8,7,1,0,8,-1,-1,-1,-1},
    {0,3,7,0,7,10,0,10,9,6,10,7,-1,-1,-1,-1},
    {7,6,10,7,10,8,8,10,9,-1,-1,-1,-1,-1,-1,-1},
    {6,8,4,11,8,6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {3,6,11,3,0,6,0,4,6,-1,-1,-1,-1,-1,-1,-1},
    {8,6,11,8,4,6,9,0,1,-1,-1,-1,-1,-1,-1,-1},
    {9,4,6,9,6,3,9,3,1,11,3,6,-1,-1,-1,-1},
    {6,8,4,6,11,8,2,10,1,-1,-1,-1,-1,-1,-1,-1},
    {1,2,10,3,0,11,0,6,11,0,4,6,-1,-1,-1,-1},
    {4,11,8,4,6,11,0,2,9,2,10,9,-1,-1,-1,-1},
    {10,9,3,10,3,2,9,4,3,11,3,6,4,6,3,-1},
    {8,2,3,8,4,2,4,6,2,-1,-1,-1,-1,-1,-1,-1},
    {0,4,2,4,6,2,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {1,9,0,2,3,4,2,4,6,4,3,8,-1,-1,-1,-1},
    {1,9,4,1,4,2,2,4,6,-1,-1,-1,-1,-1,-1,-1},
    {8,1,3,8,6,1,8,4,6,6,10,1,-1,-1,-1,-1},
    {10,1,0,10,0,6,6,0,4,-1,-1,-1,-1,-1,-1,-1},
    {4,6,3,4,3,8,6,10,3,0,3,9,10,9,3,-1},
    {10,9,4,6,10,4,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {4,9,5,7,6,11,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {0,8,3,4,9,5,11,7,6,-1,-1,-1,-1,-1,-1,-1},
    {5,0,1,5,4,0,7,6,11,-1,-1,-1,-1,-1,-1,-1},
    {11,7,6,8,3,4,3,5,4,3,1,5,-1,-1,-1,-1},
    {9,5,4,10,1,2,7,6,11,-1,-1,-1,-1,-1,-1,-1},
    {6,11,7,1,2,10,0,8,3,4,9,5,-1,-1,-1,-1},
    {7,6,11,5,4,10,4,2,10,4,0,2,-1,-1,-1,-1},
    {3,4,8,3,5,4,3,2,5,10,5,2,11,7,6,-1},
    {7,2,3,7,6,2,5,4,9,-1,-1,-1,-1,-1,-1,-1},
    {9,5,4,0,8,6,0,6,2,6,8,7,-1,-1,-1,-1},
    {3,6,2,3,7,6,1,5,0,5,4,0,-1,-1,-1,-1},
    {6,2,8,6,8,7,2,1,8,4,8,5,1,5,8,-1},
    {9,5,4,10,1,6,1,7,6,1,3,7,-1,-1,-1,-1},
    {1,6,10,1,7,6,1,0,7,8,7,0,9,5,4,-1},
    {4,0,10,4,10,5,0,3,10,6,10,7,3,7,10,-1},
    {7,6,10,7,10,8,5,4,10,4,8,10,-1,-1,-1,-1},
    {6,9,5,6,11,9,11,8,9,-1,-1,-1,-1,-1,-1,-1},
    {3,6,11,0,6,3,0,5,6,0,9,5,-1,-1,-1,-1},
    {0,11,8,0,5,11,0,1,5,5,6,11,-1,-1,-1,-1},
    {6,11,3,6,3,5,5,3,1,-1,-1,-1,-1,-1,-1,-1},
    {1,2,10,9,5,11,9,11,8,11,5,6,-1,-1,-1,-1},
    {0,11,3,0,6,11,0,9,6,5,6,9,1,2,10,-1},
    {11,8,5,11,5,6,8,0,5,10,5,2,0,2,5,-1},
    {6,11,3,6,3,5,2,10,3,10,5,3,-1,-1,-1,-1},
    {5,8,9,5,2,8,5,6,2,3,8,2,-1,-1,-1,-1},
    {9,5,6,9,6,0,0,6,2,-1,-1,-1,-1,-1,-1,-1},
    {1,5,8,1,8,0,5,6,8,3,8,2,6,2,8,-1},
    {1,5,6,2,1,6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {1,3,6,1,6,10,3,8,6,5,6,9,8,9,6,-1},
    {10,1,0,10,0,6,9,5,0,5,6,0,-1,-1,-1,-1},
    {0,3,8,5,6,10,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {10,5,6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {11,5,10,7,5,11,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {11,5,10,11,7,5,8,3,0,-1,-1,-1,-1,-1,-1,-1},
    {5,11,7,5,10,11,1,9,0,-1,-1,-1,-1,-1,-1,-1},
    {10,7,5,10,11,7,9,8,1,8,3,1,-1,-1,-1,-1},
    {11,1,2,11,7,1,7,5,1,-1,-1,-1,-1,-1,-1,-1},
    {0,8,3,1,2,7,1,7,5,7,2,11,-1,-1,-1,-1},
    {9,7,5,9,2,7,9,0,2,2,11,7,-1,-1,-1,-1},
    {7,5,2,7,2,11,5,9,2,3,2,8,9,8,2,-1},
    {2,5,10,2,3,5,3,7,5,-1,-1,-1,-1,-1,-1,-1},
    {8,2,0,8,5,2,8,7,5,10,2,5,-1,-1,-1,-1},
    {9,0,1,5,10,3,5,3,7,3,10,2,-1,-1,-1,-1},
    {9,8,2,9,2,1,8,7,2,10,2,5,7,5,2,-1},
    {1,3,5,3,7,5,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {0,8,7,0,7,1,1,7,5,-1,-1,-1,-1,-1,-1,-1},
    {9,0,3,9,3,5,5,3,7,-1,-1,-1,-1,-1,-1,-1},
    {9,8,7,5,9,7,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {5,8,4,5,10,8,10,11,8,-1,-1,-1,-1,-1,-1,-1},
    {5,0,4,5,11,0,5,10,11,11,3,0,-1,-1,-1,-1},
    {0,1,9,8,4,10,8,10,11,10,4,5,-1,-1,-1,-1},
    {10,11,4,10,4,5,11,3,4,9,4,1,3,1,4,-1},
    {2,5,1,2,8,5,2,11,8,4,5,8,-1,-1,-1,-1},
    {0,4,11,0,11,3,4,5,11,2,11,1,5,1,11,-1},
    {0,2,5,0,5,9,2,11,5,4,5,8,11,8,5,-1},
    {9,4,5,2,11,3,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {2,5,10,3,5,2,3,4,5,3,8,4,-1,-1,-1,-1},
    {5,10,2,5,2,4,4,2,0,-1,-1,-1,-1,-1,-1,-1},
    {3,10,2,3,5,10,3,8,5,4,5,8,0,1,9,-1},
    {5,10,2,5,2,4,1,9,2,9,4,2,-1,-1,-1,-1},
    {8,4,5,8,5,3,3,5,1,-1,-1,-1,-1,-1,-1,-1},
    {0,4,5,1,0,5,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {8,4,5,8,5,3,9,0,5,0,3,5,-1,-1,-1,-1},
    {9,4,5,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {4,11,7,4,9,11,9,10,11,-1,-1,-1,-1,-1,-1,-1},
    {0,8,3,4,9,7,9,11,7,9,10,11,-1,-1,-1,-1},
    {1,10,11,1,11,4,1,4,0,7,4,11,-1,-1,-1,-1},
    {3,1,4,3,4,8,1,10,4,7,4,11,10,11,4,-1},
    {4,11,7,9,11,4,9,2,11,9,1,2,-1,-1,-1,-1},
    {9,7,4,9,11,7,9,1,11,2,11,1,0,8,3,-1},
    {11,7,4,11,4,2,2,4,0,-1,-1,-1,-1,-1,-1,-1},
    {11,7,4,11,4,2,8,3,4,3,2,4,-1,-1,-1,-1},
    {2,9,10,2,7,9,2,3,7,7,4,9,-1,-1,-1,-1},
    {9,10,7,9,7,4,10,2,7,8,7,0,2,0,7,-1},
    {3,7,10,3,10,2,7,4,10,1,10,0,4,0,10,-1},
    {1,10,2,8,7,4,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {4,9,1,4,1,7,7,1,3,-1,-1,-1,-1,-1,-1,-1},
    {4,9,1,4,1,7,0,8,1,8,7,1,-1,-1,-1,-1},
    {4,0,3,7,4,3,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {4,8,7,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {9,10,8,10,11,8,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {3,0,9,3,9,11,11,9,10,-1,-1,-1,-1,-1,-1,-1},
    {0,1,10,0,10,8,8,10,11,-1,-1,-1,-1,-1,-1,-1},
    {3,1,10,11,3,10,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {1,2,11,1,11,9,9,11,8,-1,-1,-1,-1,-1,-1,-1},
    {3,0,9,3,9,11,1,2,9,2,11,9,-1,-1,-1,-1},
    {0,2,11,8,0,11,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {3,2,11,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {2,3,8,2,8,10,10,8,9,-1,-1,-1,-1,-1,-1,-1},
    {9,10,2,0,9,2,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {2,3,8,2,8,10,0,1,8,1,10,8,-1,-1,-1,-1},
    {1,10,2,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {1,3,8,9,1,8,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {0,9,1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {0,3,8,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
    {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
};

// Marching-cube corner -> (di,dj,dk) grid offsets (matches kEdgeCorners).
constexpr int kCornerOffset[8][3] = {
    {0,0,0},{1,0,0},{1,0,1},{0,0,1},
    {0,1,0},{1,1,0},{1,1,1},{0,1,1},
};

} // namespace

void SurfaceRepr::rebuildIfNeeded(const MolObject& mol) {
    auto ctx = makeContext(mol, ReprType::Surface);
    const auto& atoms = ctx.atoms;

    // ---- cache key over everything that affects geometry or colour ---------
    std::uint64_t key = kFnvOffset;
    fnvFold(key, static_cast<std::uint64_t>(mode_));
    fnvFoldFloat(key, probe_);
    fnvFoldFloat(key, resolution_);
    fnvFoldFloat(key, scale_);
    fnvFoldFloat(key, smoothness_);
    fnvFoldFloat(key, isoValue_);
    fnvFold(key, static_cast<std::uint64_t>(mol.colorScheme()));
    fnvFold(key, atoms.size());
    for (int i = 0; i < static_cast<int>(atoms.size()); ++i) {
        if (!ctx.visible(i)) continue;
        const auto& a = atoms[i];
        fnvFold(key, static_cast<std::uint64_t>(i));
        fnvFoldFloat(key, a.x); fnvFoldFloat(key, a.y); fnvFoldFloat(key, a.z);
        fnvFold(key, static_cast<std::uint64_t>(ctx.colorFor(i)));
    }
    if (cacheValid_ && key == cacheKey_) return;

    cacheKey_ = key;
    cacheValid_ = true;
    mesh_.clear();

    // ---- collect visible atoms (radius = scale*vdW) ------------------------
    struct Blob { float x, y, z; float r; int idx; };
    std::vector<Blob> blobs;
    blobs.reserve(atoms.size());
    for (int i = 0; i < static_cast<int>(atoms.size()); ++i) {
        if (!ctx.visible(i)) continue;
        const auto& a = atoms[i];
        float r = scale_ * vdwRadius(a.element);
        if (r < 0.1f) r = 0.1f;
        blobs.push_back({a.x, a.y, a.z, r, i});
    }
    if (blobs.empty()) return;

    const bool gaussian = (mode_ == Mode::Gaussian);
    // probe enters the geometry only for SES/SAS; SES collapses to vdW at p≈0.
    const float probe = (mode_ == Mode::Ses || mode_ == Mode::Sas)
                            ? probe_ : 0.0f;
    const bool ses = (mode_ == Mode::Ses && probe > 0.05f);

    // Gaussian kernel: contribution exp(-k*(d^2/r^2-1)) drops below ~0.02 at
    // d = r*sqrt(1 + 3.9/k). Geometric modes reach out to one probe past vdW.
    const float k = smoothness_;
    const float gaussCutFactor = std::sqrt(1.0f + 3.9f / k);

    // ---- bounding box, padded so the grid border is outside the surface ----
    float minX = blobs[0].x, minY = blobs[0].y, minZ = blobs[0].z;
    float maxX = minX, maxY = minY, maxZ = minZ;
    float maxR = 0.0f;
    for (const auto& b : blobs) {
        minX = std::min(minX, b.x); maxX = std::max(maxX, b.x);
        minY = std::min(minY, b.y); maxY = std::max(maxY, b.y);
        minZ = std::min(minZ, b.z); maxZ = std::max(maxZ, b.z);
        maxR = std::max(maxR, b.r);
    }
    float pad = gaussian ? (maxR * gaussCutFactor + 1.0f)
                         : (maxR + probe + 3.0f * resolution_);
    minX -= pad; minY -= pad; minZ -= pad;
    maxX += pad; maxY += pad; maxZ += pad;

    // ---- grid sizing, coarsened if it would exceed the cell budget ---------
    float spacing = resolution_;
    constexpr std::int64_t kMaxCells = 8000000;  // ~32 MB per float grid
    auto dimsFor = [&](float sp, int& nx, int& ny, int& nz) {
        nx = static_cast<int>((maxX - minX) / sp) + 2;
        ny = static_cast<int>((maxY - minY) / sp) + 2;
        nz = static_cast<int>((maxZ - minZ) / sp) + 2;
    };
    int nx, ny, nz;
    dimsFor(spacing, nx, ny, nz);
    std::int64_t cells = static_cast<std::int64_t>(nx) * ny * nz;
    if (cells > kMaxCells) {
        spacing *= std::cbrt(static_cast<float>(cells) /
                             static_cast<float>(kMaxCells));
        dimsFor(spacing, nx, ny, nz);
    }
    if (nx < 2 || ny < 2 || nz < 2) return;

    const std::size_t nCells = static_cast<std::size_t>(nx) * ny * nz;
    auto gidx = [nx, ny](int i, int j, int kk) {
        return (static_cast<std::size_t>(kk) * ny + j) * nx + i;
    };
    std::vector<float> field(nCells, gaussian ? 0.0f : 1e9f);
    std::vector<int> owner(nCells, -1);

    // For each blob, the grid sub-box of cells it can influence.
    auto blobBox = [&](const Blob& b, float cut,
                       int& i0, int& i1, int& j0, int& j1, int& k0, int& k1) {
        i0 = std::max(0, static_cast<int>((b.x - cut - minX) / spacing));
        i1 = std::min(nx - 1, static_cast<int>((b.x + cut - minX) / spacing) + 1);
        j0 = std::max(0, static_cast<int>((b.y - cut - minY) / spacing));
        j1 = std::min(ny - 1, static_cast<int>((b.y + cut - minY) / spacing) + 1);
        k0 = std::max(0, static_cast<int>((b.z - cut - minZ) / spacing));
        k1 = std::min(nz - 1, static_cast<int>((b.z + cut - minZ) / spacing) + 1);
    };

    float iso;
    if (gaussian) {
        // Sum of Gaussian blobs; iso=isoValue_, surface at scale*vdW.
        std::vector<float> ownerVal(nCells, 0.0f);
        for (const auto& b : blobs) {
            int i0, i1, j0, j1, k0, k1;
            blobBox(b, b.r * gaussCutFactor, i0, i1, j0, j1, k0, k1);
            float invr2 = 1.0f / (b.r * b.r);
            for (int kk = k0; kk <= k1; ++kk) {
                float dz = (minZ + kk * spacing) - b.z;
                for (int j = j0; j <= j1; ++j) {
                    float dy = (minY + j * spacing) - b.y;
                    float dyz2 = dy * dy + dz * dz;
                    for (int i = i0; i <= i1; ++i) {
                        float dx = (minX + i * spacing) - b.x;
                        float e = k - k * (dx * dx + dyz2) * invr2;
                        if (e < -4.0f) continue;
                        float c = std::exp(e);
                        std::size_t g = gidx(i, j, kk);
                        field[g] += c;
                        if (c > ownerVal[g]) { ownerVal[g] = c; owner[g] = b.idx; }
                    }
                }
            }
        }
        iso = isoValue_;
    } else {
        // Signed distance to the nearest atom surface: nearVdw = min(|p-c|-r).
        // Reuse `field` as nearVdw, then convert per mode below.
        for (const auto& b : blobs) {
            int i0, i1, j0, j1, k0, k1;
            blobBox(b, b.r + probe + 2.0f * spacing, i0, i1, j0, j1, k0, k1);
            for (int kk = k0; kk <= k1; ++kk) {
                float dz = (minZ + kk * spacing) - b.z;
                for (int j = j0; j <= j1; ++j) {
                    float dy = (minY + j * spacing) - b.y;
                    float dyz2 = dy * dy + dz * dz;
                    for (int i = i0; i <= i1; ++i) {
                        float dx = (minX + i * spacing) - b.x;
                        float sd = std::sqrt(dx * dx + dyz2) - b.r;
                        std::size_t g = gidx(i, j, kk);
                        if (sd < field[g]) { field[g] = sd; owner[g] = b.idx; }
                    }
                }
            }
        }

        if (!ses) {
            // vdW (iso=0) or SAS (iso=-probe): inside ⇔ nearVdw <= probe.
            // Flip sign so marching-cubes "inside = field>=iso" holds.
            for (std::size_t g = 0; g < nCells; ++g) field[g] = -field[g];
            iso = -probe;  // 0 for plain vdW (probe==0)
        } else {
            // SES: a cell is a probe centre if nearVdw>=probe ("accessible").
            // distAcc(p) = distance to nearest accessible cell; the molecule
            // (SES interior) is everything farther than `probe` from any probe
            // centre, so iso=probe carves out exactly what the probe reaches.
            std::vector<float> nearVdw = std::move(field);
            field.assign(nCells, 1e9f);
            // One linear pass to a byte mask, so the per-cell 7-neighbour
            // boundary test below is contiguous byte reads instead of repeated
            // float compares + index math.
            std::vector<char> acc(nCells);
            for (std::size_t g = 0; g < nCells; ++g)
                acc[g] = nearVdw[g] >= probe ? 1 : 0;
            int boxCells = static_cast<int>(probe / spacing) + 1;
            for (int kk = 0; kk < nz; ++kk) {
                for (int j = 0; j < ny; ++j) {
                    for (int i = 0; i < nx; ++i) {
                        std::size_t g0 = gidx(i, j, kk);
                        if (!acc[g0]) continue;
                        field[g0] = 0.0f;  // probe centre: exterior
                        // Only boundary probe centres carve the surface.
                        bool boundary =
                            i == 0 || i == nx - 1 || j == 0 || j == ny - 1 ||
                            kk == 0 || kk == nz - 1 ||
                            !acc[gidx(i - 1, j, kk)] || !acc[gidx(i + 1, j, kk)] ||
                            !acc[gidx(i, j - 1, kk)] || !acc[gidx(i, j + 1, kk)] ||
                            !acc[gidx(i, j, kk - 1)] || !acc[gidx(i, j, kk + 1)];
                        if (!boundary) continue;
                        float qx = minX + i * spacing;
                        float qy = minY + j * spacing;
                        float qz = minZ + kk * spacing;
                        int i0 = std::max(0, i - boxCells), i1 = std::min(nx - 1, i + boxCells);
                        int j0 = std::max(0, j - boxCells), j1 = std::min(ny - 1, j + boxCells);
                        int k0 = std::max(0, kk - boxCells), k1 = std::min(nz - 1, kk + boxCells);
                        for (int z = k0; z <= k1; ++z) {
                            float dz = (minZ + z * spacing) - qz;
                            for (int y = j0; y <= j1; ++y) {
                                float dy = (minY + y * spacing) - qy;
                                float dyz2 = dy * dy + dz * dz;
                                for (int x = i0; x <= i1; ++x) {
                                    std::size_t g = gidx(x, y, z);
                                    if (acc[g]) continue;  // skip accessible cells
                                    float dx = (minX + x * spacing) - qx;
                                    float d = std::sqrt(dx * dx + dyz2);
                                    if (d < field[g]) field[g] = d;
                                }
                            }
                        }
                    }
                }
            }
            iso = probe;
        }
    }

    marchingCubes(field, owner, nx, ny, nz, minX, minY, minZ, spacing, iso,
                  ctx, blobs[0].idx);
}

void SurfaceRepr::marchingCubes(const std::vector<float>& field,
                                const std::vector<int>& owner,
                                int nx, int ny, int nz,
                                float minX, float minY, float minZ,
                                float spacing, float iso,
                                const RenderContext& ctx, int fallbackAtom) {
    auto gidx = [nx, ny](int i, int j, int kk) {
        return (static_cast<std::size_t>(kk) * ny + j) * nx + i;
    };

    mesh_.reserve(field.size() / 64 + 16);
    for (int kk = 0; kk < nz - 1; ++kk) {
        for (int j = 0; j < ny - 1; ++j) {
            for (int i = 0; i < nx - 1; ++i) {
                float val[8];
                int cubeIndex = 0;
                for (int c = 0; c < 8; ++c) {
                    val[c] = field[gidx(i + kCornerOffset[c][0],
                                        j + kCornerOffset[c][1],
                                        kk + kCornerOffset[c][2])];
                    if (val[c] >= iso) cubeIndex |= (1 << c);
                }
                int edges = kEdgeTable[cubeIndex];
                if (edges == 0) continue;

                // Interpolate a vertex (position + owner) on each crossed edge.
                float vx[12], vy[12], vz[12];
                int vOwner[12];
                for (int e = 0; e < 12; ++e) {
                    if (!(edges & (1 << e))) continue;
                    int ca = kEdgeCorners[e][0], cb = kEdgeCorners[e][1];
                    const int* oa = kCornerOffset[ca];
                    const int* ob = kCornerOffset[cb];
                    float xa = minX + (i + oa[0]) * spacing;
                    float ya = minY + (j + oa[1]) * spacing;
                    float za = minZ + (kk + oa[2]) * spacing;
                    float xb = minX + (i + ob[0]) * spacing;
                    float yb = minY + (j + ob[1]) * spacing;
                    float zb = minZ + (kk + ob[2]) * spacing;
                    float va = val[ca], vb = val[cb];
                    float t = (std::abs(vb - va) > 1e-6f)
                                  ? (iso - va) / (vb - va) : 0.5f;
                    vx[e] = xa + t * (xb - xa);
                    vy[e] = ya + t * (yb - ya);
                    vz[e] = za + t * (zb - za);
                    // Colour by the nearer corner's owning atom.
                    std::size_t go = (va >= vb)
                        ? gidx(i + oa[0], j + oa[1], kk + oa[2])
                        : gidx(i + ob[0], j + ob[1], kk + ob[2]);
                    vOwner[e] = owner[go];
                }

                const int* tri = kTriTable[cubeIndex];
                for (int t = 0; tri[t] != -1; t += 3) {
                    int e0 = tri[t], e1 = tri[t + 1], e2 = tri[t + 2];
                    int ownerIdx = vOwner[e0] >= 0 ? vOwner[e0] : fallbackAtom;
                    WorldTri wt;
                    wt.x[0] = vx[e0]; wt.y[0] = vy[e0]; wt.z[0] = vz[e0];
                    wt.x[1] = vx[e1]; wt.y[1] = vy[e1]; wt.z[1] = vz[e1];
                    wt.x[2] = vx[e2]; wt.y[2] = vy[e2]; wt.z[2] = vz[e2];
                    wt.atomIdx = ownerIdx;
                    wt.colorPair = ctx.colorFor(ownerIdx);
                    mesh_.push_back(wt);
                }
            }
        }
    }
}

void SurfaceRepr::render(const MolObject& mol, const Camera& cam,
                         Canvas& canvas) {
    if (!mol.visible() || !mol.reprVisible(ReprType::Surface)) return;

    rebuildIfNeeded(mol);
    if (mesh_.empty()) return;

    int cw = canvas.subW(), ch = canvas.subH();

    std::vector<TriangleSpan> batch;
    batch.reserve(mesh_.size());
    for (const auto& wt : mesh_) {
        TriangleSpan span;
        bool anyOnscreen = false;
        for (int v = 0; v < 3; ++v) {
            float sx, sy, depth;
            cam.projectCached(wt.x[v], wt.y[v], wt.z[v], sx, sy, depth);
            span.x[v] = sx; span.y[v] = sy; span.z[v] = depth;
            if (sx >= 0 && sx < cw && sy >= 0 && sy < ch) anyOnscreen = true;
        }
        if (!anyOnscreen) continue;
        span.colorPair = wt.colorPair;
        span.atomIdx = wt.atomIdx;
        batch.push_back(span);
    }
    if (!batch.empty()) {
        canvas.drawTriangleBatch(batch.data(), batch.size());
    }
}

} // namespace molterm
