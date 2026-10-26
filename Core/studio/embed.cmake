# Turns a text file into a C++ char array: cmake -DIN=ui.html -DOUT=ui.cpp -DNAME=kStudioHtml -P embed.cmake
# (a byte list, not a string literal: compilers cap literal length).
file(READ "${IN}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n" bytes "${bytes}")
file(WRITE "${OUT}" "// Generated from ${IN} by embed.cmake: do not edit.\nstatic const unsigned char bytes[] = {\n${bytes}0x00};\nextern const char* const ${NAME};\nconst char* const ${NAME} = reinterpret_cast<const char*>(bytes);\n")
