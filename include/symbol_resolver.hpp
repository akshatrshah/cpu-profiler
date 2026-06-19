#pragma once
/*
 * symbol_resolver.hpp — Maps raw virtual addresses → human-readable symbols
 *
 * Portable compilation:
 *   On Linux  → parses /proc/<pid>/maps + reads ELF .symtab directly (no libelf).
 *   On macOS  → stub that always returns the hex address.
 *     (macOS uses Mach-O + dtrace/DTrace for profiling; this profiler's
 *      ptrace/perf_event engine is Linux-only anyway.)
 *
 * All unit-testable logic (MapRegion, ElfSym, binary-search, etc.) compiles
 * on every platform so the test suite runs anywhere.
 */

#include "types.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// ELF types — Linux ships elf.h; on macOS we define only what we need.
#ifdef __linux__
#  include <elf.h>
#else
// ── Minimal ELF64 definitions for non-Linux builds ──────────────────────────
#  include <cstdint>
using Elf64_Half  = uint16_t;
using Elf64_Word  = uint32_t;
using Elf64_Xword = uint64_t;
using Elf64_Off   = uint64_t;
using Elf64_Addr  = uint64_t;

static constexpr int ELFMAG0    = 0x7f;
static constexpr char ELFMAG[]  = "\177ELF";
static constexpr int SELFMAG    = 4;
static constexpr int EI_CLASS   = 4;
static constexpr int ELFCLASS64 = 2;
static constexpr uint32_t SHT_SYMTAB  = 2;
static constexpr uint32_t SHT_DYNSYM  = 11;
static constexpr uint8_t  STT_FUNC    = 2;

inline uint8_t ELF64_ST_TYPE(uint8_t i) { return i & 0xf; }

struct Elf64_Ehdr {
    unsigned char e_ident[16];
    Elf64_Half    e_type, e_machine;
    Elf64_Word    e_version;
    Elf64_Addr    e_entry;
    Elf64_Off     e_phoff, e_shoff;
    Elf64_Word    e_flags;
    Elf64_Half    e_ehsize, e_phentsize, e_phnum;
    Elf64_Half    e_shentsize, e_shnum, e_shstrndx;
};
struct Elf64_Shdr {
    Elf64_Word  sh_name, sh_type;
    Elf64_Xword sh_flags, sh_addr, sh_offset, sh_size;
    Elf64_Word  sh_link, sh_info;
    Elf64_Xword sh_addralign, sh_entsize;
};
struct Elf64_Sym {
    Elf64_Word  st_name;
    uint8_t     st_info, st_other;
    Elf64_Half  st_shndx;
    Elf64_Addr  st_value;
    Elf64_Xword st_size;
};
#endif // !__linux__

namespace profiler {

// ---------------------------------------------------------------------------
// One entry from /proc/<pid>/maps  (or a synthetic entry in tests)
// ---------------------------------------------------------------------------
struct MapRegion {
    uint64_t    start  = 0;
    uint64_t    end    = 0;
    uint64_t    offset = 0;   // file offset of mapping start
    std::string path;
    bool        exec   = false;
};

// ---------------------------------------------------------------------------
// Lightweight ELF symbol extracted from .symtab / .dynsym
// ---------------------------------------------------------------------------
struct ElfSym {
    uint64_t    addr = 0;
    uint64_t    size = 0;
    std::string name;
};

// ---------------------------------------------------------------------------
// SymbolResolver
// ---------------------------------------------------------------------------
class SymbolResolver {
public:
    explicit SymbolResolver(pid_t pid) : pid_(pid) {}

    // Resolve a virtual address → Frame.  Never throws.
    Frame resolve(uint64_t addr) {
        Frame f;
        f.address = addr;

        const MapRegion *region = find_region(addr);
        if (!region) {
            load_maps();
            region = find_region(addr);
        }

        if (!region || region->path.empty()) {
            f.symbol = hex(addr);
            f.binary = "[unknown]";
            return f;
        }

        f.binary = basename_of(region->path);
        f.offset = addr - region->start + region->offset;

        f.symbol = lookup_elf_symbol(region->path, f.offset);
        if (f.symbol.empty()) {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s+0x%" PRIx64, f.binary.c_str(), f.offset);
            f.symbol = buf;
        }
        return f;
    }

    void reload() { maps_.clear(); syms_.clear(); load_maps(); }

    const std::vector<MapRegion>& maps() const { return maps_; }

private:
    pid_t                    pid_;
    std::vector<MapRegion>   maps_;
    std::unordered_map<std::string, std::vector<ElfSym>> syms_;

    // ── /proc/<pid>/maps  (Linux) or /proc/<pid>/maps stub (macOS) ──────────
    void load_maps() {
        maps_.clear();

#ifdef __linux__
        std::ifstream f("/proc/" + std::to_string(pid_) + "/maps");
        if (!f) return;

        std::string line;
        while (std::getline(f, line)) {
            MapRegion r;
            char perms[8]={}, dev[16]={}, path[512]={};
            unsigned long inode = 0;
            int n = sscanf(line.c_str(), "%lx-%lx %7s %lx %15s %lu %511[^\n]",
                           &r.start, &r.end, perms, &r.offset, dev, &inode, path);
            if (n < 4) continue;
            r.exec = (perms[2] == 'x');
            if (n >= 7 && path[0] != '\0') {
                char *p = path; while (*p == ' ') ++p;
                r.path = p;
            }
            maps_.push_back(r);
        }
#else
        // macOS: /proc doesn't exist.  Symbol resolution is unavailable;
        // the profiler engine itself also won't run on macOS.
        (void)pid_;
#endif

        std::sort(maps_.begin(), maps_.end(),
                  [](const MapRegion &a, const MapRegion &b){ return a.start < b.start; });
    }

    const MapRegion* find_region(uint64_t addr) const {
        if (maps_.empty()) return nullptr;
        auto it = std::upper_bound(maps_.begin(), maps_.end(), addr,
            [](uint64_t a, const MapRegion &r){ return a < r.start; });
        if (it == maps_.begin()) return nullptr;
        --it;
        if (addr >= it->start && addr < it->end) return &*it;
        return nullptr;
    }

    // ── ELF symbol lookup ────────────────────────────────────────────────────
    std::string lookup_elf_symbol(const std::string &path, uint64_t file_offset) {
        auto &table = get_syms(path);
        if (table.empty()) return {};

        auto it = std::upper_bound(table.begin(), table.end(), file_offset,
            [](uint64_t off, const ElfSym &s){ return off < s.addr; });
        if (it == table.begin()) return {};
        --it;

        if (file_offset >= it->addr &&
            (it->size == 0 || file_offset < it->addr + it->size)) {
            if (it->size == 0) {
                char buf[256];
                snprintf(buf, sizeof(buf), "%s+0x%" PRIx64,
                         it->name.c_str(), file_offset - it->addr);
                return buf;
            }
            return it->name;
        }
        return {};
    }

    const std::vector<ElfSym>& get_syms(const std::string &path) {
        auto it = syms_.find(path);
        if (it != syms_.end()) return it->second;
        auto &table = syms_[path];
        load_elf_syms(path, table);
        std::sort(table.begin(), table.end(),
                  [](const ElfSym &a, const ElfSym &b){ return a.addr < b.addr; });
        return table;
    }

    // Reads ELF .symtab / .dynsym from a file on disk.
    // The ELF struct definitions above mean this compiles on macOS too,
    // but it will simply return nothing (macOS binaries are Mach-O, not ELF).
    static void load_elf_syms(const std::string &path, std::vector<ElfSym> &out) {
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) return;

        struct stat st{};
        if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(Elf64_Ehdr)) {
            close(fd); return;
        }

        void *base = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (base == MAP_FAILED) return;

        auto *eh = reinterpret_cast<const Elf64_Ehdr*>(base);
        if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) { munmap(base, st.st_size); return; }
        if (eh->e_ident[EI_CLASS] != ELFCLASS64)        { munmap(base, st.st_size); return; }

        const char *bytes = reinterpret_cast<const char*>(base);

        for (int pass = 0; pass < 2; ++pass) {
            uint32_t target_type = (pass == 0) ? SHT_SYMTAB : SHT_DYNSYM;
            for (int i = 0; i < eh->e_shnum; ++i) {
                off_t sh_off = (off_t)(eh->e_shoff + (unsigned)i * eh->e_shentsize);
                if (sh_off + (off_t)sizeof(Elf64_Shdr) > st.st_size) break;
                auto *sh = reinterpret_cast<const Elf64_Shdr*>(bytes + sh_off);
                if (sh->sh_type != target_type) continue;
                if (sh->sh_link >= (unsigned)eh->e_shnum) continue;

                off_t stsh_off = (off_t)(eh->e_shoff + sh->sh_link * eh->e_shentsize);
                if (stsh_off + (off_t)sizeof(Elf64_Shdr) > st.st_size) continue;
                auto *stsh = reinterpret_cast<const Elf64_Shdr*>(bytes + stsh_off);

                const char *strtab = bytes + stsh->sh_offset;
                const char *symtab = bytes + sh->sh_offset;
                uint64_t nsyms = sh->sh_size / sh->sh_entsize;

                for (uint64_t s = 0; s < nsyms; ++s) {
                    auto *sym = reinterpret_cast<const Elf64_Sym*>(
                        symtab + s * sh->sh_entsize);
                    if (ELF64_ST_TYPE(sym->st_info) != STT_FUNC) continue;
                    if (sym->st_value == 0) continue;
                    if (sym->st_name  == 0) continue;

                    uint32_t name_off = sym->st_name;
                    if (stsh->sh_offset + name_off >= (uint64_t)st.st_size) continue;

                    ElfSym e;
                    e.addr = sym->st_value;
                    e.size = sym->st_size;
                    e.name = strtab + name_off;
                    out.push_back(e);
                }
                break;
            }
        }
        munmap(base, st.st_size);
    }

    static std::string hex(uint64_t addr) {
        char buf[32]; snprintf(buf, sizeof(buf), "0x%" PRIx64, addr); return buf;
    }
    static std::string basename_of(const std::string &p) {
        auto pos = p.rfind('/');
        return (pos == std::string::npos) ? p : p.substr(pos + 1);
    }
};

} // namespace profiler