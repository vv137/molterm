# Generate a C header with the font embedded as a byte array.
# Usage: cmake -DINPUT=font.ttf -DOUTPUT=font_data.h -P embed_font.cmake

file(READ "${INPUT}" HEX_CONTENT HEX)
string(LENGTH "${HEX_CONTENT}" HEX_LEN)
math(EXPR BYTE_LEN "${HEX_LEN} / 2")

set(OUTPUT_CONTENT "#pragma once\n#include <cstddef>\n#include <cstdint>\n\n")
string(APPEND OUTPUT_CONTENT "// Auto-generated from ${INPUT}\n")
string(APPEND OUTPUT_CONTENT "static const unsigned char kEmbeddedFont[] = {\n")

set(i 0)
set(LINE "")
while(i LESS HEX_LEN)
    string(SUBSTRING "${HEX_CONTENT}" ${i} 2 BYTE)
    string(APPEND LINE "0x${BYTE},")
    math(EXPR i "${i} + 2")
    math(EXPR COL "(${i} / 2) % 16")
    if(COL EQUAL 0)
        string(APPEND OUTPUT_CONTENT "    ${LINE}\n")
        set(LINE "")
    endif()
endwhile()

if(NOT LINE STREQUAL "")
    string(APPEND OUTPUT_CONTENT "    ${LINE}\n")
endif()

string(APPEND OUTPUT_CONTENT "};\n")
string(APPEND OUTPUT_CONTENT "static const size_t kEmbeddedFontSize = ${BYTE_LEN};\n")

file(WRITE "${OUTPUT}" "${OUTPUT_CONTENT}")
