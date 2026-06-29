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

// Accumulated unresolved symbol count across all elfLoad calls since elfResetCounts()
static int g_unresolved_count = 0;
int elfGetUnresolvedCount() { return g_unresolved_count; }

// First JIT failure code seen since elfResetCounts() (0 = all OK so far)
static uint32_t g_last_svc_perm_code = 0;
uint32_t elfGetLastSvcPermCode() { return g_last_svc_perm_code; }

void elfResetCounts() {
    g_unresolved_count = 0;
    g_last_svc_perm_code = 0;
}

// ─── elfRunCtors ──────────────────────────────────────────────────────────────
// Run DT_INIT_ARRAY constructors stored by elfLoad.  Logs each entry before
// calling it (and flushes via compatLog) so the crash site is visible in the
// log when the Switch dies inside a constructor.
void elfRunCtors(LoadedSo* so) {
    if (!so || !so->init_arr || so->init_arr_count == 0) return;
    size_t sl = so->path.rfind('/');
    const char* soname = (sl != std::string::npos)
                         ? so->path.c_str() + sl + 1 : so->path.c_str();
    compatLogFmt("ELF: %s: running %zu constructors", soname, so->init_arr_count);
    for (size_t k = 0; k < so->init_arr_count; k++) {
        LoadedSo::InitFn fn = so->init_arr[k];
        if (fn && fn != (LoadedSo::InitFn)(uintptr_t)-1) {
            // compatLog flushes after every write — crash site will show here
            compatLogFmt("ELF: ctor[%zu/%zu] @%p", k + 1, so->init_arr_count, (void*)fn);
            fn();
            compatLogFmt("ELF: ctor[%zu/%zu] OK", k + 1, so->init_arr_count);
        }
    }
    compatLogFmt("ELF: %s: all constructors done", soname);
}

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
// write_base: where to write relocation results (RW mapping)
// exec_base:  address values to store in GOT entries (RX mapping)
// These differ when using JIT dual-mapping; they're equal in the heap fallback.
static void applyRela(LoadedSo* so, const Elf64_Rela* relas, size_t count,
                      uint8_t* write_base, uint8_t* exec_base,
                      uint8_t* write_alloc, size_t alloc_size,
                      uint64_t strsz, const char* tag) {
    for (size_t i = 0; i < count; i++) {
        const Elf64_Rela& r = relas[i];
        uint32_t sym_idx = ELF64_R_SYM(r.r_info);
        uint32_t type    = ELF64_R_TYPE(r.r_info);

        compatLogFmt("%s[%zu/%zu] type=%u sym_idx=%u off=0x%llx addend=0x%llx",
                     tag, i + 1, count, type, sym_idx,
                     (unsigned long long)r.r_offset, (long long)r.r_addend);

        if (r.r_offset < so->min_vaddr) continue;
        uint8_t* target_ptr = write_base + r.r_offset;
        if (target_ptr < write_alloc || target_ptr + 8 > write_alloc + alloc_size) {
            compatLogFmt("%s[%zu/%zu] WARN target 0x%llx out of stage bounds — skipped",
                         tag, i + 1, count, (unsigned long long)r.r_offset);
            continue;
        }
        uint64_t* target = (uint64_t*)target_ptr;

        if (type == R_AARCH64_RELATIVE) {
            *target = (uint64_t)exec_base + (uint64_t)r.r_addend;
            continue;
        }

        if (!so->symtab || sym_idx == 0) continue;
        if (sym_idx >= so->sym_count) {
            compatLogFmt("%s[%zu/%zu] WARN sym_idx %u >= sym_count %u — skipped",
                         tag, i + 1, count, sym_idx, so->sym_count);
            continue;
        }

        const Elf64_Sym& sym = so->symtab[sym_idx];
        compatLogFmt("%s[%zu/%zu] sym.st_name=%u st_value=0x%llx st_size=%llu st_shndx=%u",
                     tag, i + 1, count, sym.st_name,
                     (unsigned long long)sym.st_value, (unsigned long long)sym.st_size,
                     sym.st_shndx);

        // .gnu.version sits between .dynsym and .dynstr in most ELFs, which can
        // inflate the gap-derived sym_count past the real symbol table. Guard
        // st_name against strsz before treating it as a string pointer.
        const char* sym_name = "";
        if (so->strtab) {
            if (strsz == 0 || sym.st_name < strsz) {
                sym_name = so->strtab + sym.st_name;
            } else {
                compatLogFmt("%s[%zu/%zu] WARN st_name %u >= strsz %llu — name lookup skipped",
                             tag, i + 1, count, sym.st_name, (unsigned long long)strsz);
            }
        }

        uint64_t sym_addr = 0;
        if (sym.st_shndx != SHN_UNDEF && sym.st_value != 0) {
            // Defined in this module — return its exec-side address
            sym_addr = (uint64_t)exec_base + sym.st_value;
        } else if (sym_name[0]) {
            compatLogFmt("%s[%zu/%zu] resolving \"%s\"", tag, i + 1, count, sym_name);
            sym_addr = (uint64_t)resolveSymbol(sym_name);
            if (!sym_addr) {
                compatLogFmt("ELF: unresolved: %s", sym_name);
                g_unresolved_count++;
            } else {
                compatLogFmt("%s[%zu/%zu] resolved \"%s\" -> %p",
                             tag, i + 1, count, sym_name, (void*)sym_addr);
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
                if (sym_addr && sym.st_size > 0) {
                    size_t csz = (size_t)sym.st_size;
                    if (csz > 0x10000) {
                        compatLogFmt("%s[%zu/%zu] WARN COPY size %zu capped to 0x10000",
                                     tag, i + 1, count, csz);
                        csz = 0x10000;
                    }
                    memcpy(target, (void*)sym_addr, csz);
                }
                break;
        }
        compatLogFmt("%s[%zu/%zu] OK", tag, i + 1, count);
    }
    compatLogFmt("%s: all %zu entries processed", tag, count);
}

// ─── elfLoad ──────────────────────────────────────────────────────────────────
// Does NOT reset g_unresolved_count or g_last_svc_perm_code — caller must call
// elfResetCounts() before loading a batch so counts accumulate correctly.
LoadedSo* elfLoad(const char* path) {
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

    // ── Allocate code memory via JIT API ─────────────────────────────────────
    // jitCreate() uses svcMapCodeMemory internally, which creates a dual-view
    // mapping: a writable (src_addr) side and an executable (dst_addr) side.
    // This is necessary because heap memory (memalign) cannot be made Rx via
    // svcSetMemoryPermission on Switch (returns 0xD801).
    Jit    jit_mem   = {};
    bool   using_jit = false;
    uint8_t* write_alloc = nullptr;  // writable mapping (RW)
    uint8_t* exec_alloc  = nullptr;  // executable mapping (Rx)

    Result jit_rc = jitCreate(&jit_mem, alloc_size);
    if (R_SUCCEEDED(jit_rc)) {
        Result w_rc = jitTransitionToWritable(&jit_mem);
        if (R_SUCCEEDED(w_rc)) {
            using_jit   = true;
            write_alloc = (uint8_t*)jit_mem.rw_addr;  // writable mapping
            exec_alloc  = (uint8_t*)jit_mem.rx_addr;  // executable mapping
            compatLogFmt("JIT: alloc OK write=%p exec=%p size=0x%zx",
                         (void*)write_alloc, (void*)exec_alloc, alloc_size);
        } else {
            compatLogFmt("JIT: jitTransitionToWritable failed 0x%08X", w_rc);
            jitClose(&jit_mem);
        }
    } else {
        compatLogFmt("JIT: jitCreate failed 0x%08X — falling back to heap (not executable)", jit_rc);
    }

    if (!using_jit) {
        // Heap fallback: code won't be executable, but we can still log unresolved symbols.
        write_alloc = exec_alloc = (uint8_t*)memalign(0x1000, alloc_size);
        if (!write_alloc) { free(file_data); compatLog("ELF: memalign failed"); return nullptr; }
    }

    // ── Use heap staging buffer so JIT memory is only touched once ──────────
    // All ELF processing (segment copy, relocation) happens on a heap buffer.
    // Only one bulk memcpy goes to the JIT writable region, right before
    // jitTransitionToExecutable.  This avoids random small writes to JIT pages,
    // which can fault on some Switch firmware if the mapping state is unexpected.
    uint8_t* stage = (uint8_t*)malloc(alloc_size);
    if (!stage) {
        compatLog("ELF: malloc staging buffer OOM");
        if (using_jit) jitClose(&jit_mem);
        else           free(write_alloc);
        free(file_data);
        return nullptr;
    }
    compatLog("ELF: stage alloc OK");
    memset(stage, 0, alloc_size);
    compatLog("ELF: stage zeroed");

    // stage_base: same offset math as write_base but in heap memory
    uint8_t* stage_base = stage - min_vaddr;
    uint8_t* exec_base  = exec_alloc - min_vaddr;  // exec-side addresses for GOT

    // ── Copy PT_LOAD segments into staging buffer ────────────────────────────
    for (int i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr& ph = phdrs[i];
        if (ph.p_type != PT_LOAD || ph.p_filesz == 0) continue;
        if (ph.p_offset + ph.p_filesz > fsize) {
            compatLogFmt("ELF: seg[%d] WARN offset+filesz exceeds file size — skipped", i);
            continue;
        }
        uint8_t* seg_dst = stage_base + ph.p_vaddr;
        if (seg_dst < stage || seg_dst + ph.p_filesz > stage + alloc_size) {
            compatLogFmt("ELF: seg[%d] WARN dest out of stage bounds — skipped", i);
            continue;
        }
        compatLogFmt("ELF: seg[%d] vaddr=0x%llx filesz=0x%llx memsz=0x%llx flags=0x%x",
                     i, (unsigned long long)ph.p_vaddr, (unsigned long long)ph.p_filesz,
                     (unsigned long long)ph.p_memsz, ph.p_flags);
        memcpy(seg_dst, file_data + ph.p_offset, ph.p_filesz);
    }
    compatLog("ELF: segs copied to stage");

    // ── Parse PT_DYNAMIC from staging buffer ─────────────────────────────────
    uint64_t strtab_vaddr = 0, symtab_vaddr = 0;
    uint64_t rela_vaddr = 0, rela_sz = 0;
    uint64_t jmprel_vaddr = 0, jmprel_sz = 0;
    uint64_t strsz = 0, syment = sizeof(Elf64_Sym);
    uint64_t init_arr_vaddr = 0, init_arr_sz = 0;

    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type != PT_DYNAMIC) continue;
        uint8_t* dyn_ptr = stage_base + phdrs[i].p_vaddr;
        if (dyn_ptr < stage || dyn_ptr >= stage + alloc_size) {
            compatLogFmt("ELF: PT_DYNAMIC out of stage bounds — skipping dynamic parse");
            break;
        }
        const Elf64_Dyn* dyn = (const Elf64_Dyn*)dyn_ptr;
        for (int d = 0; d < 4096 && dyn->d_tag != DT_NULL; dyn++, d++) {
            switch (dyn->d_tag) {
                case DT_STRTAB:      strtab_vaddr   = dyn->d_un.d_ptr; break;
                case DT_SYMTAB:      symtab_vaddr   = dyn->d_un.d_ptr; break;
                case DT_RELA:        rela_vaddr     = dyn->d_un.d_ptr; break;
                case DT_RELASZ:      rela_sz        = dyn->d_un.d_val; break;
                case DT_JMPREL:      jmprel_vaddr   = dyn->d_un.d_ptr; break;
                case DT_PLTRELSZ:    jmprel_sz      = dyn->d_un.d_val; break;
                case DT_STRSZ:       strsz          = dyn->d_un.d_val; break;
                case DT_SYMENT:      syment         = dyn->d_un.d_val; break;
                case DT_INIT_ARRAY:  init_arr_vaddr = dyn->d_un.d_ptr; break;
                case DT_INIT_ARRAYSZ:init_arr_sz    = dyn->d_un.d_val; break;
            }
        }
        break;
    }
    compatLogFmt("ELF: dyn: strtab=0x%llx/%llu symtab=0x%llx syment=%llu "
                 "rela=0x%llx/%llu jmprel=0x%llx/%llu init_arr=0x%llx/%llu",
                 (unsigned long long)strtab_vaddr, (unsigned long long)strsz,
                 (unsigned long long)symtab_vaddr, (unsigned long long)syment,
                 (unsigned long long)rela_vaddr,   (unsigned long long)rela_sz,
                 (unsigned long long)jmprel_vaddr, (unsigned long long)jmprel_sz,
                 (unsigned long long)init_arr_vaddr,(unsigned long long)init_arr_sz);

    // ── Build LoadedSo — strtab/symtab point into staging buffer ─────────────
    LoadedSo* so = new LoadedSo();
    so->using_jit  = using_jit;
    so->jit_mem    = jit_mem;
    so->alloc      = exec_alloc;
    so->alloc_size = alloc_size;
    so->min_vaddr  = min_vaddr;
    so->base       = exec_base;
    so->path       = path;

    uint32_t sym_count = 0;
    if (strtab_vaddr) so->strtab = (const char*)(stage_base + strtab_vaddr);
    if (symtab_vaddr && strtab_vaddr && syment) {
        so->symtab = (Elf64_Sym*)(stage_base + symtab_vaddr);
        if (strtab_vaddr > symtab_vaddr)
            sym_count = (uint32_t)((strtab_vaddr - symtab_vaddr) / syment);
        if (sym_count > 200000) sym_count = 200000;
        so->sym_count = sym_count;
    }
    compatLogFmt("ELF: so built sym_count=%u", sym_count);

    // Register now so cross-library resolution works during relocation
    g_loaded_sos.push_back(so);
    compatLog("ELF: registered");

    // ── Apply relocations to staging buffer ──────────────────────────────────
    // GOT entries store exec-side addresses; the writes go to the heap stage.
    if (rela_vaddr && rela_sz && so->symtab) {
        compatLogFmt("ELF: rela %llu entries", (unsigned long long)(rela_sz / sizeof(Elf64_Rela)));
        applyRela(so, (const Elf64_Rela*)(stage_base + rela_vaddr),
                  rela_sz / sizeof(Elf64_Rela),
                  stage_base, exec_base, stage, alloc_size, strsz, "RELA");
    }
    compatLog("ELF: rela done");
    if (jmprel_vaddr && jmprel_sz && so->symtab) {
        compatLogFmt("ELF: jmprel %llu entries", (unsigned long long)(jmprel_sz / sizeof(Elf64_Rela)));
        applyRela(so, (const Elf64_Rela*)(stage_base + jmprel_vaddr),
                  jmprel_sz / sizeof(Elf64_Rela),
                  stage_base, exec_base, stage, alloc_size, strsz, "JMPREL");
    }
    compatLog("ELF: jmprel done");

    // ── Copy strtab/symtab to heap before staging buffer is freed ────────────
    if (strtab_vaddr && strsz) {
        so->strtab_heap = (char*)malloc(strsz + 1);
        if (so->strtab_heap) {
            memcpy(so->strtab_heap, stage_base + strtab_vaddr, strsz);
            so->strtab_heap[strsz] = '\0';
            so->strtab = so->strtab_heap;
        }
    }
    if (symtab_vaddr && sym_count && syment) {
        size_t symtab_bytes = (size_t)sym_count * sizeof(Elf64_Sym);
        so->symtab_heap = (Elf64_Sym*)malloc(symtab_bytes);
        if (so->symtab_heap) {
            memcpy(so->symtab_heap, stage_base + symtab_vaddr, symtab_bytes);
            so->symtab = so->symtab_heap;
        }
    }
    compatLog("ELF: strtab/symtab copied");

    // ── Bulk-copy staging buffer into JIT writable region ────────────────────
    // Only one memcpy to JIT memory, avoiding many small random writes.
    if (using_jit) {
        compatLogFmt("ELF: copying stage->JIT rw=%p size=0x%zx", (void*)write_alloc, alloc_size);
        memcpy(write_alloc, stage, alloc_size);
        compatLog("ELF: stage->JIT copy done");
    }
    free(stage);
    compatLog("ELF: stage freed");

    // ── Transition to executable ─────────────────────────────────────────────
    uint32_t this_svc_perm_code = 0;
    if (using_jit) {
        Result exec_rc = jitTransitionToExecutable(&jit_mem);
        so->jit_mem = jit_mem;
        this_svc_perm_code = (uint32_t)exec_rc;
        if (R_FAILED(exec_rc)) {
            compatLogFmt("JIT: jitTransitionToExecutable failed: 0x%08X", exec_rc);
        } else {
            compatLog("JIT: code memory is now executable");
        }
    } else {
        this_svc_perm_code = 0xD801;  // heap fallback — not executable
    }
    // Accumulate into global (capture first failure, 0 = all OK so far)
    if (g_last_svc_perm_code == 0 && this_svc_perm_code != 0)
        g_last_svc_perm_code = this_svc_perm_code;

    // Flush CPU instruction cache over exec region
    __builtin___clear_cache((char*)exec_alloc, (char*)exec_alloc + alloc_size);

    // ── Store DT_INIT_ARRAY for deferred constructor run ────────────────────
    // Constructors are run AFTER all SOs in the batch are loaded (via elfRunCtors),
    // so cross-library symbols are available.  Only store if JIT succeeded.
    if (init_arr_vaddr && init_arr_sz && using_jit && this_svc_perm_code == 0) {
        so->init_arr       = (LoadedSo::InitFn*)(exec_base + init_arr_vaddr);
        so->init_arr_count = init_arr_sz / sizeof(LoadedSo::InitFn);
        compatLogFmt("ELF: %zu constructors deferred (run after all SOs loaded)",
                     so->init_arr_count);
    }

    free(file_data);
    compatLogFmt("ELF: loaded OK exec_base=%p sym_count=%u unresolved=%d",
                 (void*)exec_base, so->sym_count, g_unresolved_count);
    return so;
}
