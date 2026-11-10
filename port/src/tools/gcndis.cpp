// gcndis <shader.bin> [--fetch]: disassemble a dumped GCN shader.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "gpu/gcn/decoder.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: gcndis <shader.bin> [--fetch]\n");
        return 1;
    }
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
    std::vector<uint32_t> code(bytes.size() / 4);
    std::memcpy(code.data(), bytes.data(), code.size() * 4);
    const bool fetch = argc > 2 && std::string(argv[2]) == "--fetch";
    for (const auto& inst : gcn::Decode(code.data(), uint32_t(code.size()), fetch))
        std::printf("%04x: %s\n", inst.pc * 4, gcn::Disassemble(inst).c_str());
    return 0;
}
