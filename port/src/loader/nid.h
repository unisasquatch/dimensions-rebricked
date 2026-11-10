#pragma once
#include <string>
#include <string_view>

namespace loader {

// PS4 symbols are imported by NID: base64 of the first 8 bytes of
// SHA-1(name + fixed suffix), using '+' and '-' in place of '+' and '/'.
std::string ComputeNid(std::string_view name);

}  // namespace loader
