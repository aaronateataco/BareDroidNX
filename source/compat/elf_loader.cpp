#include "compat/loader.h"
#include <switch.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <malloc.h>
#include <vector>

// Log helpers (shared across compat/ via extern)
extern void compatLog(const char* msg);
extern void compatLogFmt(const char* fmt, ...);

// External shim table from shim_table.cpp
void* shimResolve(const char* name);

// Count of unresolved symbols from the most recent elfLoad call
static int g_unresolved_count = 0;
int elfGetUnresolvedCount() { return g_unresolved_count; }

// All successfully loaded .so files (for cross-library symbol resolution)
static std::vector<LoadedSo*> g_loaded_sos;

// ─── LoadedSo::findSym ────────────────────────────────────────────────────────
void* LoadedSo::findSym(const char* name) const {
    if (!symtab || !strtab) return nullptr;
    for (uint32_t i = 1; i < sym_count; i++) {
        const Elf64_Sym& s = symtab[i];
        if (s.st_shndx == SHN_UNDEF || s.st_value == 0) continue;
        const char* sname = strtab + s.st_name;
        if (strcmp(sname, name) == 0)
            return base + s.st_value;
    }
    return nullptr;
}

// ─── Global symbol resolver ───────────────────────────────────────────────────
// Searches loaded .so files first, then falls back to shim table
static void* resolveSymbol(const char* name) {
    if (!name || !name[0]) return nullptr;

    // Check already-loaded libraries (allows cross-library resolution)
    for (LoadedSo* so : g_loaded_sos) {
        void* p = so->findSym(name);
        if (p) return p;
    }

    // Fall back to our shim table (libc, GLES, EGL, libandroid, etc.)
    return shimResolve(name);
}

// ─── RELA relocation processing ───────────────────────────────────────────────
static void applyRela(LoadedSo* so, const Elf64_Rela* relas, size_t count) {
    for (size_t i = 0; i < count; i++) {
        const Elf64_Rela& r = relas[i];
        uint32_t sym_idx = ELF64_R_SYM(r.r_info);
        uint32_t type    = ELF64_R_TYPE(r.r_info);

        // Bounds check the relocation target
        if (r.r_offset < so->min_vaddr) continue;
        uint8_t* target_ptr = so->base + r.r_offset;
        if (target_ptr < so->alloc || target_ptr + 8 > so->alloc + so->alloc_size)
            continue;
        uint64_t* target = (uint64_t*)target_ptr;

        if (type == R_AARCH64_RELATIVE) {
            *target = (uint64_t)so->base + (uint64_t)r.r_addend;
            continue;
        }

        // Need a symbol for other relocation types
        if (!so->symtab || sym_idx == 0) continue;
        if (sym_idx >= so->sym_count) continue;

        const Elf64_Sym& sym = so->symtab[sym_idx];
        const char* sym_name = so->strtab ? (so->strtab + sym.st_name) : "";

        uint64_t sym_addr = 0;
        if (sym.st_shndx != SHN_UNDEF && sym.st_value != 0) {
            // Defined in this module
            sym_addr = (uint64_t)so->base + sym.st_value;
        } else if (sym_name[0]) {
            sym_addr = (uint64_t)resolveSymbol(sym_name);
            if (!sym_addr) {
                compatLogFmt("ELF: unresolved: %s", sym_name);
                g_unresolved_count++;
            }
        }

        switch (type) {
            case R_AARCH64_ABS64:
                *target = sym_addr + (uint64_t)r.r_addend;
                break;
            case R_AARCH64_GLOB_DAT:
                *target = sym_addr + (uint64_t)r.r_addend;
                break;
            case R_AARCH64_JUMP_SLOT:
                *target = sym_addr;
                break;
            case R_AARCH64_COPY:
                if (sym_addr && sym.st_size > 0)
                    memcpy(target, (void*)sym_addr, sym.st_size);
                break;
        }
    }
}

// ─── elfLoad ──────────────────────────────────────────────────────────────────
LoadedSo* elfLoad(const char* path) {
    g_unresolved_count = 0;
    compatLogFmt("ELF: loading %s", path);

    // Read the entire file
    FILE* f = fopen(path, "rb");
    if (!f) { compatLog("ELF: fopen failed"); return nullptr; }

    fseek(f, 0, SEEK_END);
    size_t fsize = (size_t)ftell(f);
    rewind(f);

    uint8_t* file_data = (uint8_t*)malloc(fsize);
    if (!file_data) { fclose(f); compatLog("ELF: OOM"); return nullptr; }
    fread(file_data, 1, fsize, f);
    fclose(f);

    // Validate ELF header
    const Elf64_Ehdr* ehdr = (const Elf64_Ehdr*)file_data;
    if (fsize < sizeof(Elf64_Ehdr) ||
        memcmp(ehdr->e_ident, "\x7f" "ELF", 4) ||
        ehdr->e_ident[4] != 2 ||           // ELFCLASS64
        ehdr->e_ident[5] != 1 ||           // ELFDATA2LSB
        ehdr->e_machine  != EM_AARCH64 ||
        ehdr->e_type     != ET_DYN) {
        free(file_data);
        compatLog("ELF: not an ARM64 shared lib");
        return nullptr;
    }

    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
        free(file_data);
        compatLog("ELF: no program headers");
        return nullptr;
    }

    // Walk PT_LOAD segments to find the virtual address span
    const Elf64_Phdr* phdrs = (const Elf64_Phdr*)(file_data + ehdr->e_phoff);
    uint64_t min_vaddr = UINT64_MAX, max_vaddr = 0;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        if (phdrs[i].p_vaddr < min_vaddr) min_vaddr = phdrs[i].p_vaddr;
        uint64_t end = phdrs[i].p_vaddr + phdrs[i].p_memsz;
        if (end > max_vaddr) max_vaddr = end;
    }
    if (min_vaddr == UINT64_MAX) {
        free(file_data);
        compatLog("ELF: no PT_LOAD segments");
        return nullptr;
    }

    size_t alloc_size = (size_t)ALIGN_UP(max_vaddr - min_vaddr, 0x1000);
    uint8_t* alloc = (uint8_t*)memalign(0x1000, alloc_size);
    if (!alloc) {
        free(file_data);
        compatLog("ELF: memalign failed");
        return nullptr;
    }
    memset(alloc, 0, alloc_size);

    // base + p_vaddr == pointer into our allocation
    uint8_t* base = alloc - min_vaddr;

    // Copy PT_LOAD segments
    for (int i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr& ph = phdrs[i];
        if (ph.p_type != PT_LOAD || ph.p_filesz == 0) continue;
        if (ph.p_offset + ph.p_filesz > fsize) continue;
        memcpy(base + ph.p_vaddr, file_data + ph.p_offset, ph.p_filesz);
    }

    LoadedSo* so = new LoadedSo();
    so->alloc      = alloc;
    so->alloc_size = alloc_size;
    so->min_vaddr  = min_vaddr;
    so->base       = base;
    so->path       = path;

    // Parse PT_DYNAMIC
    uint64_t strtab_vaddr = 0, symtab_vaddr = 0;
    uint64_t rela_vaddr = 0, rela_sz = 0;
    uint64_t jmprel_vaddr = 0, jmprel_sz = 0;
    uint64_t strsz = 0, syment = sizeof(Elf64_Sym);
    uint64_t init_arr_vaddr = 0, init_arr_sz = 0;

    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type != PT_DYNAMIC) continue;
        const Elf64_Dyn* dyn = (const Elf64_Dyn*)(base + phdrs[i].p_vaddr);

        for (; dyn->d_tag != DT_NULL; dyn++) {
            switch (dyn->d_tag) {
                case DT_STRTAB:   strtab_vaddr  = dyn->d_un.d_ptr; break;
                case DT_SYMTAB:   symtab_vaddr  = dyn->d_un.d_ptr; break;
                case DT_RELA:     rela_vaddr    = dyn->d_un.d_ptr; break;
                case DT_RELASZ:   rela_sz       = dyn->d_un.d_val; break;
                case DT_JMPREL:   jmprel_vaddr  = dyn->d_un.d_ptr; break;
                case DT_PLTRELSZ: jmprel_sz     = dyn->d_un.d_val; break;
                case DT_STRSZ:    strsz         = dyn->d_un.d_val; break;
                case DT_SYMENT:   syment        = dyn->d_un.d_val; break;
                case DT_INIT_ARRAY:    init_arr_vaddr = dyn->d_un.d_ptr; break;
                case DT_INIT_ARRAYSZ:  init_arr_sz    = dyn->d_un.d_val; break;
            }
        }
        break;
    }

    if (strtab_vaddr) so->strtab = (const char*)(base + strtab_vaddr);
    if (symtab_vaddr && strtab_vaddr && syment) {
        so->symtab = (Elf64_Sym*)(base + symtab_vaddr);
        // Estimate count: symtab runs up to strtab, roughly
        if (strtab_vaddr > symtab_vaddr)
            so->sym_count = (uint32_t)((strtab_vaddr - symtab_vaddr) / syment);
        if (so->sym_count > 200000) so->sym_count = 200000;
    }

    // Register this so now so cross-library resolution works during relocation
    g_loaded_sos.push_back(so);

    // Apply relocations
    if (rela_vaddr && rela_sz && so->symtab) {
        applyRela(so, (const Elf64_Rela*)(base + rela_vaddr),
                  rela_sz / sizeof(Elf64_Rela));
    }
    if (jmprel_vaddr && jmprel_sz && so->symtab) {
        applyRela(so, (const Elf64_Rela*)(base + jmprel_vaddr),
                  jmprel_sz / sizeof(Elf64_Rela));
    }

    // Set memory permissions per segment (need Rx for code pages)
    for (int i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr& ph = phdrs[i];
        if (ph.p_type != PT_LOAD) continue;

        uint32_t perm = 0;
        if (ph.p_flags & PF_R) perm |= Perm_R;
        if (ph.p_flags & PF_W) perm |= Perm_W;
        if (ph.p_flags & PF_X) perm |= Perm_X;

        // Only call svc when we need something other than default Rw
        if (!(ph.p_flags & PF_X)) continue;

        uintptr_t seg_start = (uintptr_t)ALIGN_DOWN((uintptr_t)(base + ph.p_vaddr), 0x1000);
        size_t    seg_size  = (size_t)ALIGN_UP(ph.p_memsz + (ph.p_vaddr - ALIGN_DOWN(ph.p_vaddr, 0x1000)), 0x1000);
        Result res = svcSetMemoryPermission((void*)seg_start, seg_size, perm);
        compatLogFmt("ELF: svcSetMemPerm code seg: 0x%08X", res);
    }

    // Flush CPU instruction cache over the entire loaded region
    __builtin___clear_cache((char*)alloc, (char*)alloc + alloc_size);

    // Run DT_INIT_ARRAY constructors
    if (init_arr_vaddr && init_arr_sz) {
        typedef void(*InitFn)();
        InitFn* arr = (InitFn*)(base + init_arr_vaddr);
        size_t count = init_arr_sz / sizeof(InitFn);
        for (size_t k = 0; k < count; k++) {
            if (arr[k] && arr[k] != (InitFn)-1)
                arr[k]();
        }
    }

    free(file_data);
    compatLogFmt("ELF: loaded OK, base=%p sym_count=%u", (void*)base, so->sym_count);
    return so;
}
