#include "loader/orbis_elf.h"

#include <cstring>
#include <fstream>

#include "host/memory.h"

namespace loader {
namespace {

constexpr uint32_t PT_LOAD = 1;
constexpr uint32_t PT_DYNAMIC = 2;
constexpr uint32_t PT_TLS = 7;
constexpr uint32_t PT_SCE_RELRO = 0x61000010;
constexpr uint32_t PT_SCE_DYNLIBDATA = 0x61000000;

constexpr int64_t DT_NULL = 0;
constexpr int64_t DT_SCE_NEEDED_MODULE = 0x6100000F;
constexpr int64_t DT_SCE_IMPORT_LIB = 0x61000015;
constexpr int64_t DT_SCE_JMPREL = 0x61000029;
constexpr int64_t DT_SCE_PLTRELSZ = 0x6100002D;
constexpr int64_t DT_SCE_RELA = 0x6100002F;
constexpr int64_t DT_SCE_RELASZ = 0x61000031;
constexpr int64_t DT_SCE_STRTAB = 0x61000035;
constexpr int64_t DT_SCE_STRSZ = 0x61000037;
constexpr int64_t DT_SCE_SYMTAB = 0x61000039;
constexpr int64_t DT_SCE_SYMTABSZ = 0x6100003F;

constexpr uint32_t R_X86_64_64 = 1;
constexpr uint32_t R_X86_64_GLOB_DAT = 6;
constexpr uint32_t R_X86_64_JUMP_SLOT = 7;
constexpr uint32_t R_X86_64_RELATIVE = 8;

constexpr uint64_t kPageSize = 0x4000;

struct Phdr { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; };

template <typename T>
T Read(const std::vector<uint8_t>& buf, uint64_t off) {
    T v;
    std::memcpy(&v, buf.data() + off, sizeof(T));
    return v;
}

// Module and library ids in symbol names are written in the NID base64 alphabet.
uint16_t DecodeId(const std::string& s) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    uint16_t v = 0;
    for (char c : s) v = uint16_t(v * 64 + (std::strchr(kAlphabet, c) - kAlphabet));
    return v;
}

uint64_t AlignUp(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

}  // namespace

std::string OrbisElf::Str(uint64_t off) const {
    return reinterpret_cast<const char*>(file_.data() + dynlib_off_ + strtab_ + off);
}

std::string OrbisElf::ModuleName(const std::string& code) const {
    const uint16_t id = DecodeId(code);
    for (const auto& m : modules_) if (m.id == id) return m.name;
    return "?";
}

std::string OrbisElf::LibraryName(const std::string& code) const {
    const uint16_t id = DecodeId(code);
    for (const auto& l : libraries_) if (l.id == id) return l.name;
    return "?";
}

bool OrbisElf::Load(const std::string& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { error = "cannot open " + path; return false; }
    file_.assign(std::istreambuf_iterator<char>(in), {});
    if (file_.size() < 64 || std::memcmp(file_.data(), "\x7f" "ELF", 4) != 0) {
        error = "not an ELF (unwrap the SELF with self2elf.py first)";
        return false;
    }

    entry_ = Read<uint64_t>(file_, 0x18);
    const uint64_t phoff = Read<uint64_t>(file_, 0x20);
    const uint16_t phnum = Read<uint16_t>(file_, 0x38);
    std::vector<Phdr> phdrs(phnum);
    for (uint16_t i = 0; i < phnum; ++i) phdrs[i] = Read<Phdr>(file_, phoff + i * sizeof(Phdr));

    const Phdr* dynamic = nullptr;
    for (const auto& ph : phdrs) {
        if (ph.type == PT_LOAD || ph.type == PT_SCE_RELRO)
            image_size_ = std::max<uint64_t>(image_size_, AlignUp(ph.vaddr + ph.memsz, kPageSize));
        if (ph.type == PT_LOAD && (ph.flags & 1)) exec_ranges_.emplace_back(ph.vaddr, ph.filesz);
        if (ph.type == PT_TLS) tls_ = {ph.vaddr, ph.filesz, ph.memsz, std::max<uint64_t>(ph.align, 1)};
        if (ph.type == PT_DYNAMIC) dynamic = &ph;
        if (ph.type == PT_SCE_DYNLIBDATA) dynlib_off_ = ph.offset;
    }
    if (!dynamic || !dynlib_off_) { error = "missing dynamic or dynlibdata segment"; return false; }

    base_ = host::AllocateImage(image_size_);
    if (!base_) { error = "image allocation failed"; return false; }
    for (const auto& ph : phdrs) {
        if (ph.type == PT_LOAD || ph.type == PT_SCE_RELRO)
            std::memcpy(base_ + ph.vaddr, file_.data() + ph.offset, ph.filesz);
    }

    // First pass collects table locations; names need the string table, so a second pass follows.
    std::vector<std::pair<int64_t, uint64_t>> entries;
    for (uint64_t off = dynamic->offset; off < dynamic->offset + dynamic->filesz; off += 16) {
        const int64_t tag = Read<int64_t>(file_, off);
        const uint64_t val = Read<uint64_t>(file_, off + 8);
        if (tag == DT_NULL) break;
        entries.emplace_back(tag, val);
        switch (tag) {
            case DT_SCE_STRTAB: strtab_ = val; break;
            case DT_SCE_STRSZ: strsz_ = val; break;
            case DT_SCE_SYMTAB: symtab_ = val; break;
            case DT_SCE_SYMTABSZ: symtabsz_ = val; break;
            case DT_SCE_RELA: rela_ = val; break;
            case DT_SCE_RELASZ: relasz_ = val; break;
            case DT_SCE_JMPREL: jmprel_ = val; break;
            case DT_SCE_PLTRELSZ: jmprelsz_ = val; break;
        }
    }
    for (const auto& [tag, val] : entries) {
        // Low 32 bits: name offset. Top 16 bits: id used in symbol-name suffixes.
        if (tag == DT_SCE_NEEDED_MODULE) modules_.push_back({uint16_t(val >> 48), Str(val & 0xFFFFFFFF)});
        if (tag == DT_SCE_IMPORT_LIB) libraries_.push_back({uint16_t(val >> 48), Str(val & 0xFFFFFFFF)});
    }
    return true;
}

bool OrbisElf::Relocate(const Resolver& resolve, std::string& error) {
    auto apply = [&](uint64_t table, uint64_t size) -> bool {
        for (uint64_t off = 0; off < size; off += sizeof(Rela)) {
            const auto r = Read<Rela>(file_, dynlib_off_ + table + off);
            const uint32_t type = uint32_t(r.info);
            const uint32_t sym_index = uint32_t(r.info >> 32);
            auto* slot = reinterpret_cast<uint64_t*>(base_ + r.offset);

            if (type == R_X86_64_RELATIVE) {
                *slot = uint64_t(base_) + r.addend;
                ++relative_relocs_;
                continue;
            }
            if (type != R_X86_64_64 && type != R_X86_64_GLOB_DAT && type != R_X86_64_JUMP_SLOT) {
                error = "unhandled relocation type " + std::to_string(type);
                return false;
            }

            const auto sym = Read<Sym>(file_, dynlib_off_ + symtab_ + sym_index * sizeof(Sym));
            uint64_t target;
            if (sym.shndx != 0) {
                target = uint64_t(base_) + sym.value;  // defined in this image
            } else {
                // Imported names look like "NID#lib#mod".
                const std::string name = Str(sym.name);
                const size_t a = name.find('#'), b = name.find('#', a + 1);
                Import imp;
                imp.nid = name.substr(0, a);
                imp.library = LibraryName(name.substr(a + 1, b - a - 1));
                imp.module = ModuleName(name.substr(b + 1));
                imp.sym_type = sym.info & 0xF;
                imp.slot = r.offset;
                target = resolve(imp);
                imports_.push_back(std::move(imp));
            }
            *slot = target + (type == R_X86_64_64 ? r.addend : 0);
        }
        return true;
    };
    return apply(rela_, relasz_) && apply(jmprel_, jmprelsz_);
}

size_t OrbisElf::PatchTcbReads(uint32_t gs_displacement) {
    static constexpr uint8_t kFsRead[9] = {0x64, 0x48, 0x8B, 0x04, 0x25, 0, 0, 0, 0};
    size_t count = 0;
    for (const auto& [vaddr, size] : exec_ranges_) {
        uint8_t* p = base_ + vaddr;
        for (uint64_t i = 0; i + sizeof(kFsRead) <= size; ++i) {
            if (std::memcmp(p + i, kFsRead, sizeof(kFsRead)) != 0) continue;
            p[i] = 0x65;  // gs segment prefix; same length, so no code moves
            std::memcpy(p + i + 5, &gs_displacement, 4);
            ++count;
            i += sizeof(kFsRead) - 1;
        }
    }
    return count;
}

bool OrbisElf::MakeExecutable() {
    return host::SetProtection(base_, image_size_, host::kRead | host::kWrite | host::kExec);
}

}  // namespace loader
