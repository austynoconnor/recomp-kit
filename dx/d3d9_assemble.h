#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

namespace d9sh {
// Assemble bounded DX9 vertex 1.1/2.0 and pixel 2.0 assembly. Failure clears bytecode and identifies
// the source line; unsupported syntax is never substituted with another shader.
bool assemble(std::string_view source, std::vector<uint32_t> &words, std::string &error);
} // namespace d9sh
void d3dx9_assembler_register();
