# cmake -DINPUT=<file> -DOUTPUT=<header> -DNAME=<identifier> -P EmbedFile.cmake
file(READ ${INPUT} hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE ${OUTPUT}
    "// Generated from ${INPUT}. Do not edit.\n"
    "#pragma once\n"
    "alignas(4) inline constexpr unsigned char ${NAME}[] = {${bytes}};\n")
