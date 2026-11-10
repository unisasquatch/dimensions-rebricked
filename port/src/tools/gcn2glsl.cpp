// gcn2glsl <vs|ps> <shader.bin> [fetch.bin]: translate a dumped GCN shader to GLSL on stdout.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "gpu/gcn/translator.h"

namespace {

std::vector<uint32_t> Load(const char* path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
    std::vector<uint32_t> words(bytes.size() / 4 + 1, 0xBF810000);  // pad with s_endpgm
    std::memcpy(words.data(), bytes.data(), bytes.size() / 4 * 4);
    return words;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: gcn2glsl <vs|ps> <shader.bin> [fetch.bin]\n");
        return 1;
    }
    const gcn::Stage stage = std::string(argv[1]) == "vs" ? gcn::Stage::Vertex : gcn::Stage::Pixel;
    const auto code = Load(argv[2]);
    std::vector<uint32_t> fetch;
    if (argc > 3) fetch = Load(argv[3]);
    const auto program = gcn::Build(stage, code.data(), fetch.empty() ? nullptr : fetch.data(), 16);
    gcn::ShaderKey key{stage, 16, 0x2};
    key.image_types.assign(program.image_sites, gcn::ImageType::Tex2D);
    std::fputs(gcn::GenerateGlsl(program, key).c_str(), stdout);
    return 0;
}
