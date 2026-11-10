#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace loader {

struct ModuleRef {
    uint16_t id;
    std::string name;
};

struct Import {
    std::string nid;
    std::string library;   // resolved from the "#lib" suffix of the symbol name
    std::string module;    // resolved from the "#mod" suffix
    uint32_t sym_type;     // STT_FUNC, STT_OBJECT, ...
    uint64_t slot;         // image offset the resolved address is written to
};

// A PS4 executable mapped into host memory with its relocations applied.
// Imports are bound through `resolve`, which returns a host address (or 0).
class OrbisElf {
public:
    using Resolver = std::function<uint64_t(const Import&)>;

    bool Load(const std::string& path, std::string& error);
    bool Relocate(const Resolver& resolve, std::string& error);

    // Rewrites every `mov rax, fs:[0]` (thread control block read) in executable segments
    // to `mov rax, gs:[disp]`, where disp addresses a host per-thread slot. Returns the count.
    size_t PatchTcbReads(uint32_t gs_displacement);
    // Makes the whole image executable (development builds; tightened later).
    bool MakeExecutable();

    struct TlsInfo { uint64_t vaddr = 0, filesz = 0, memsz = 0, align = 1; };
    const TlsInfo& tls() const { return tls_; }

    uint8_t* base() const { return base_; }
    size_t image_size() const { return image_size_; }
    uint64_t entry() const { return entry_; }
    const std::vector<ModuleRef>& modules() const { return modules_; }
    const std::vector<ModuleRef>& libraries() const { return libraries_; }
    const std::vector<Import>& imports() const { return imports_; }
    size_t relative_relocs() const { return relative_relocs_; }

private:
    struct Rela { uint64_t offset, info; int64_t addend; };
    struct Sym { uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size; };

    std::string Str(uint64_t off) const;
    std::string ModuleName(const std::string& code) const;
    std::string LibraryName(const std::string& code) const;

    std::vector<uint8_t> file_;
    uint8_t* base_ = nullptr;
    size_t image_size_ = 0;
    uint64_t entry_ = 0;
    TlsInfo tls_;
    std::vector<std::pair<uint64_t, uint64_t>> exec_ranges_;  // vaddr, size

    // Dynamic-linking tables live in the SCE_DYNLIBDATA segment of the file.
    uint64_t dynlib_off_ = 0;
    uint64_t strtab_ = 0, strsz_ = 0;
    uint64_t symtab_ = 0, symtabsz_ = 0;
    uint64_t rela_ = 0, relasz_ = 0;
    uint64_t jmprel_ = 0, jmprelsz_ = 0;

    std::vector<ModuleRef> modules_;
    std::vector<ModuleRef> libraries_;
    std::vector<Import> imports_;
    size_t relative_relocs_ = 0;
};

}  // namespace loader
