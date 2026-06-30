#include "compat/loader.h"
#include <switch.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <malloc.h>
#include <vector>
#include <setjmp.h>
#include <signal.h>
#include <switch/arm/thread_context.h>

// Log helpers — declared before the exception handler so it can use them.
extern void compatLog(const char* msg);
extern void compatLogFmt(const char* fmt, ...);
extern void compatUiLog(const char* msg);
extern void compatUiSetPct(int pct);

// ─── Shared crash recovery ────────────────────────────────────────────────────
jmp_buf           g_recover_jmp;
volatile bool     g_in_recover  = false;
volatile int      g_recover_sig = 0;
volatile uint32_t g_recover_esr = 0;
volatile uint64_t g_recover_pc  = 0;

extern "C" void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    uint32_t esr = ctx->esr;

    if (g_in_recover) {
        g_recover_sig = (int)ctx->error_desc;
        g_recover_esr = esr;
        g_recover_pc  = ctx->pc.x;
        longjmp(g_recover_jmp, 1);
    }
    extern ThreadExceptionDump __nx_exceptiondump;
    __nx_exceptiondump = *ctx;
    svcReturnFromException(0xf801);
}

static void ctor_crash_handler(int sig) {
    if (g_in_recover) { g_recover_sig = sig; longjmp(g_recover_jmp, 1); }
}

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
void elfRunCtors(LoadedSo* so, ProgressCb cb) {
    if (!so || !so->init_arr || so->init_arr_count == 0) return;
    size_t sl = so->path.rfind('/');
    const char* soname = (sl != std::string::npos)
                         ? so->path.c_str() + sl + 1 : so->path.c_str();

    // libapplovin-native-crash-reporter registers real SIGSEGV/SIGBUS handlers
    // and reads /proc/self/maps — both crash on Switch.  It's non-essential
    // (crash reporting only), so skip its constructors entirely.
    if (strstr(soname, "applovin") != nullptr) {
        compatLogFmt("ELF: %s: SKIP constructors (crash-reporter, not needed)", soname);
        compatUiLog("applovin: skip ctors (crash-reporter)");
        return;
    }

    signal(SIGSEGV, ctor_crash_handler);
    signal(SIGBUS,  ctor_crash_handler);
    signal(SIGILL,  ctor_crash_handler);

    int  failed = 0, skipped = 0, ok = 0;

    // DT_INIT runs before DT_INIT_ARRAY (same as Android linker order)
    if (so->init_fn) {
        compatLogFmt("ELF: %s: DT_INIT @%p", soname, (void*)so->init_fn);
        g_in_recover = true; g_recover_sig = 0; g_recover_esr = 0;
        if (setjmp(g_recover_jmp) == 0) {
            so->init_fn();
            g_in_recover = false;
            compatLog("ELF: DT_INIT OK");
        } else {
            g_in_recover = false;
            compatLogFmt("ELF: DT_INIT FAULT sig=%d — skipped", g_recover_sig);
        }
    }

    compatLogFmt("ELF: %s: running %zu constructors", soname, so->init_arr_count);
    {
        char ub[80];
        snprintf(ub, sizeof(ub), "%s: running %zu ctors", soname, so->init_arr_count);
        compatUiLog(ub);
    }
    compatUiSetPct(60);

    const size_t n = so->init_arr_count;
    // Emit a UI update every ~50 ctors and at the end
    const size_t ui_interval = (n > 50) ? (n / 8) : n;

    for (size_t k = 0; k < n; k++) {
        LoadedSo::InitFn fn = so->init_arr[k];
        if (!fn || fn == (LoadedSo::InitFn)(uintptr_t)-1) { skipped++; continue; }

        compatLogFmt("ELF: ctor[%zu/%zu] @%p", k+1, n, (void*)fn);
        g_in_recover = true; g_recover_sig = 0; g_recover_esr = 0;
        if (setjmp(g_recover_jmp) == 0) {
            fn();
            g_in_recover = false;
            compatLogFmt("ELF: ctor[%zu/%zu] OK", k + 1, n);
            ok++;
        } else {
            g_in_recover = false;
            compatLogFmt("ELF: ctor[%zu/%zu] FAULT sig=%d esr=0x%08x pc=%p — skipped",
                         k + 1, n, g_recover_sig, g_recover_esr, (void*)g_recover_pc);
            failed++;
        }

        if ((k + 1) % ui_interval == 0 || k + 1 == n) {
            char ub[80];
            snprintf(ub, sizeof(ub), "%s ctor[%zu/%zu] ok=%d fault=%d",
                     soname, k + 1, n, ok, failed);
            compatUiLog(ub);
            int pct = 60 + (int)(20 * (k + 1) / n);
            compatUiSetPct(pct);
            if (cb) cb("Running constructors", ub);
        }
    }
    signal(SIGSEGV, SIG_DFL);
    signal(SIGBUS,  SIG_DFL);
    signal(SIGILL,  SIG_DFL);
    compatLogFmt("ELF: %s: ctors done ok=%d failed=%d skipped=%d",
                 soname, ok, failed, skipped);
    {
        char ub[80];
        snprintf(ub, sizeof(ub), "%s: ctors done ok=%d failed=%d", soname, ok, failed);
        compatUiLog(ub);
    }
}

// All successfully loaded .so files (for cross-library symbol resolution)
static std::vector<LoadedSo*> g_loaded_sos;

// ─── LoadedSo::findSym ────────────────────────────────────────────────────────
void* LoadedSo::findSym(const char* name) const {
    if (!symtab || !strtab) return nullptr;
    for (uint32_t i = 1; i < sym_count; i++) {
        const Elf64_Sym& s = symtab[i];
        if (s.st_shndx == SHN_UNDEF || s.st_value == 0) continue;
        // sym_count is derived from the gap between .dynsym and .dynstr, which
        // often includes .gnu.version bytes interpreted as fake Elf64_Sym entries.
        // Those fake entries can have wild st_name values that walk off the end
        // of the string table — guard before dereferencing.
        if (strsz > 0 && (uint64_t)s.st_name >= strsz) continue;
        const char* sname = strtab + s.st_name;
        if (strcmp(sname, name) == 0) {
            if (data_alloc && data_vaddr > 0 && s.st_value >= data_vaddr)
                return data_alloc + (s.st_value - data_vaddr);
            return base + s.st_value;
        }
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

// ─── ADRP data-segment redirect ───────────────────────────────────────────────
// Scans code_buf (the writable copy of the code segment, size code_size words),
// finds ADRP instructions whose natural runtime target falls in the phantom data
// range [phantom_data_start, phantom_data_end), and rewrites them to land in the
// separate data_jit allocation at data_rx instead.  Both allocations are in the
// same svcMapCodeMemory code-region VA band, so the delta fits in ADRP ±4 GB.
static void patchAdrpToDataJit(uint8_t* code_buf, size_t code_size,
                                uint64_t code_rx,
                                uint64_t phantom_data_start,
                                uint64_t phantom_data_end,
                                uint64_t data_rx) {
    uint32_t* words  = (uint32_t*)code_buf;
    size_t    nwords = code_size / 4;
    int       patched = 0, skipped = 0;
    for (size_t i = 0; i < nwords; i++) {
        uint32_t insn = words[i];
        if ((insn & 0x9F000000u) != 0x90000000u) continue;  // not ADRP

        uint64_t pc      = code_rx + (uint64_t)i * 4;
        uint64_t pc_page = pc & ~0xfffULL;

        int64_t immhi = (int64_t)((insn >> 5) & 0x7ffff);
        int64_t immlo = (int64_t)((insn >> 29) & 3);
        int64_t imm21 = (immhi << 2) | immlo;
        if (imm21 & (1LL << 20)) imm21 -= (1LL << 21);

        uint64_t tgt_page = (uint64_t)((int64_t)pc_page + imm21 * 4096LL);
        if (tgt_page < phantom_data_start || tgt_page >= phantom_data_end) continue;

        uint64_t off_in_data = tgt_page - phantom_data_start;
        uint64_t new_tgt     = data_rx + off_in_data;
        int64_t  new_imm21   = ((int64_t)new_tgt - (int64_t)pc_page) / 4096LL;

        if (new_imm21 < -(1 << 20) || new_imm21 >= (1 << 20)) { ++skipped; continue; }

        uint32_t nlo = (uint32_t)(new_imm21 & 3);
        uint32_t nhi = (uint32_t)((new_imm21 >> 2) & 0x7ffff);
        words[i] = (insn & 0x9F00001Fu) | (nlo << 29) | (nhi << 5);
        ++patched;
    }
    compatLogFmt("ADRP→data_jit: %d patched %d skipped (code_rx=%p data_rx=0x%llx phantom=0x%llx..0x%llx)",
                 patched, skipped, (void*)code_rx,
                 (unsigned long long)data_rx,
                 (unsigned long long)phantom_data_start,
                 (unsigned long long)phantom_data_end);
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

    // Walk PT_LOAD segments to find the virtual address span and first data segment
    const Elf64_Phdr* phdrs = (const Elf64_Phdr*)(file_data + ehdr->e_phoff);
    uint64_t min_vaddr      = UINT64_MAX, max_vaddr = 0;
    uint64_t data_seg_vaddr = UINT64_MAX;  // vaddr of first writable (PF_W) segment
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        if (phdrs[i].p_vaddr < min_vaddr) min_vaddr = phdrs[i].p_vaddr;
        uint64_t end = phdrs[i].p_vaddr + phdrs[i].p_memsz;
        if (end > max_vaddr) max_vaddr = end;
        if ((phdrs[i].p_flags & PF_W) && phdrs[i].p_vaddr < data_seg_vaddr)
            data_seg_vaddr = phdrs[i].p_vaddr;
    }
    if (min_vaddr == UINT64_MAX) {
        free(file_data);
        compatLog("ELF: no PT_LOAD segments");
        return nullptr;
    }

    size_t alloc_size = (size_t)ALIGN_UP(max_vaddr - min_vaddr, 0x1000);

    // Page-aligned split between code and data segments.
    // data_off_pg == 0 means no separate data segment (single JIT allocation).
    uint64_t data_off_pg = 0;
    if (data_seg_vaddr != UINT64_MAX && data_seg_vaddr > min_vaddr)
        data_off_pg = ALIGN_DOWN(data_seg_vaddr - min_vaddr, 0x1000);
    size_t code_jit_size = (data_off_pg > 0) ? (size_t)data_off_pg : alloc_size;
    size_t data_jit_size = alloc_size - code_jit_size;

    // ── Allocate JIT regions ──────────────────────────────────────────────────
    // We create TWO svcMapCodeMemory allocations when there is a writable data
    // segment: one for code (promoted to Rx) and one for data (kept Rw).
    // Both rx_addrs land in the same code-region VA band of the process, so
    // they are close enough for ADRP ±4 GB patching to work.
    Jit      code_jit = {}, data_jit = {};
    bool     using_jit      = false;
    bool     using_data_jit = false;
    uint8_t* code_write = nullptr;  // code_jit writable side
    uint8_t* code_exec  = nullptr;  // code_jit rx_addr (→ Rx after transition)
    uint8_t* data_write = nullptr;  // data_jit writable side
    uint8_t* data_exec  = nullptr;  // data_jit rx_addr (stays Rw permanently)

    Result jit_rc = jitCreate(&code_jit, code_jit_size);
    if (R_SUCCEEDED(jit_rc)) {
        Result w_rc = jitTransitionToWritable(&code_jit);
        if (R_SUCCEEDED(w_rc)) {
            using_jit  = true;
            code_write = (uint8_t*)code_jit.rw_addr;
            code_exec  = (uint8_t*)code_jit.rx_addr;
            compatLogFmt("JIT: code write=%p exec=%p size=0x%zx",
                         (void*)code_write, (void*)code_exec, code_jit_size);
        } else {
            compatLogFmt("JIT: code jitTransitionToWritable 0x%08X", w_rc);
            jitClose(&code_jit);
        }
    } else {
        compatLogFmt("JIT: code jitCreate 0x%08X — heap fallback", (uint32_t)jit_rc);
    }

    if (!using_jit) {
        code_write = code_exec = (uint8_t*)memalign(0x1000, alloc_size);
        if (!code_write) { free(file_data); compatLog("ELF: memalign failed"); return nullptr; }
    } else if (data_jit_size > 0) {
        Result d_rc = jitCreate(&data_jit, data_jit_size);
        if (R_SUCCEEDED(d_rc)) {
            d_rc = jitTransitionToWritable(&data_jit);
            if (R_SUCCEEDED(d_rc)) {
                using_data_jit = true;
                data_write = (uint8_t*)data_jit.rw_addr;
                data_exec  = (uint8_t*)data_jit.rx_addr;
                compatLogFmt("JIT: data write=%p exec=%p size=0x%zx (stays Rw)",
                             (void*)data_write, (void*)data_exec, data_jit_size);
            } else {
                compatLogFmt("JIT: data jitTransitionToWritable 0x%08X", (uint32_t)d_rc);
                jitClose(&data_jit);
            }
        } else {
            compatLogFmt("JIT: data jitCreate 0x%08X — data writes will fault", (uint32_t)d_rc);
        }
    }

    // ── Heap staging buffer ───────────────────────────────────────────────────
    uint8_t* stage = (uint8_t*)malloc(alloc_size);
    if (!stage) {
        compatLog("ELF: malloc staging buffer OOM");
        if (using_jit) { jitClose(&code_jit); if (using_data_jit) jitClose(&data_jit); }
        else           free(code_write);
        free(file_data);
        return nullptr;
    }
    compatLog("ELF: stage alloc OK");
    memset(stage, 0, alloc_size);
    compatLog("ELF: stage zeroed");

    // exec_base: used for GOT entries that reference CODE symbols.
    // Data symbols are at data_exec+offset (handled after relocations via remapping).
    uint8_t* stage_base = stage - min_vaddr;
    uint8_t* exec_base  = code_exec - min_vaddr;

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
    uint64_t init_fn_vaddr = 0;
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
                case DT_INIT:        init_fn_vaddr  = dyn->d_un.d_ptr; break;
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
    so->using_jit   = using_jit;
    so->jit_mem     = code_jit;
    so->alloc       = code_exec;
    so->write_alloc = code_write;
    so->data_alloc  = data_exec;   // nullptr if single-jit
    so->alloc_size  = alloc_size;
    so->min_vaddr   = min_vaddr;
    // data_vaddr stores the page-aligned vaddr base of the data_jit allocation,
    // used by findSym to route data-segment symbols to data_exec.
    so->data_vaddr  = using_data_jit ? (min_vaddr + data_off_pg) : 0;
    so->base        = exec_base;
    so->path        = path;

    uint32_t sym_count = 0;
    if (strtab_vaddr) so->strtab = (const char*)(stage_base + strtab_vaddr);
    so->strsz = strsz;
    if (symtab_vaddr && strtab_vaddr && syment) {
        so->symtab = (Elf64_Sym*)(stage_base + symtab_vaddr);
        if (strtab_vaddr > symtab_vaddr)
            sym_count = (uint32_t)((strtab_vaddr - symtab_vaddr) / syment);
        if (sym_count > 200000) sym_count = 200000;
        so->sym_count = sym_count;
    }
    compatLogFmt("ELF: so built sym_count=%u strsz=%llu", sym_count, (unsigned long long)strsz);

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

    // ── Post-relocation: remap phantom data pointers → data_exec ─────────────
    // Relocations above used exec_base = code_exec − min_vaddr, so any
    // R_AARCH64_RELATIVE / local-symbol address that falls in the data segment
    // will have been written as (code_exec + data_vaddr_offset), which is a
    // "phantom" address (code pages, not the real data_jit allocation).
    // Scan the data portion of stage and fix up those 64-bit pointers.
    if (using_data_jit) {
        uint64_t ph_start = (uint64_t)code_exec + data_off_pg;
        uint64_t ph_end   = (uint64_t)code_exec + alloc_size;
        uint64_t* scan    = (uint64_t*)(stage + data_off_pg);
        size_t    nscan   = data_jit_size / 8;
        int       remapped = 0;
        for (size_t i = 0; i < nscan; i++) {
            uint64_t v = scan[i];
            if (v >= ph_start && v < ph_end) {
                scan[i] = (uint64_t)data_exec + (v - ph_start);
                ++remapped;
            }
        }
        compatLogFmt("data-ptr remap: %d entries (phantom=0x%llx..0x%llx → data_rx=%p)",
                     remapped, (unsigned long long)ph_start,
                     (unsigned long long)ph_end, (void*)data_exec);

        // ── ADRP patch: redirect code→data ADRP instructions ─────────────
        // ADRP instructions in the code segment will compute addresses in the
        // phantom range (code_exec + data_off_pg ...). Patch them to land in
        // data_exec instead, which is the actual Rw data allocation.
        patchAdrpToDataJit(stage, code_jit_size,
                           (uint64_t)code_exec,
                           ph_start, ph_end,
                           (uint64_t)data_exec);
    }

    // ── Copy stage → JIT regions ─────────────────────────────────────────────
    if (using_jit) {
        compatLogFmt("ELF: copy code→JIT write=%p size=0x%zx", (void*)code_write, code_jit_size);
        memcpy(code_write, stage, code_jit_size);
        if (using_data_jit) {
            compatLogFmt("ELF: copy data→JIT write=%p size=0x%zx", (void*)data_write, data_jit_size);
            memcpy(data_write, stage + data_off_pg, data_jit_size);
        }
    }
    free(stage);
    compatLog("ELF: stage freed");

    // ── Transition code_jit to Rx; data_jit stays Rw ─────────────────────────
    uint32_t this_svc_perm_code = 0;
    if (using_jit) {
        Result exec_rc = jitTransitionToExecutable(&code_jit);
        so->jit_mem = code_jit;
        this_svc_perm_code = (uint32_t)exec_rc;
        if (R_FAILED(exec_rc))
            compatLogFmt("JIT: code jitTransitionToExecutable failed 0x%08X", exec_rc);
        else
            compatLogFmt("JIT: code Rx OK%s",
                         using_data_jit ? "; data_jit stays Rw" : "");
    } else {
        this_svc_perm_code = 0xD801;
    }
    if (g_last_svc_perm_code == 0 && this_svc_perm_code != 0)
        g_last_svc_perm_code = this_svc_perm_code;

    __builtin___clear_cache((char*)code_exec, (char*)code_exec + code_jit_size);

    // ── Store DT_INIT / DT_INIT_ARRAY for deferred constructor run ──────────
    // Helper: convert a vaddr to its runtime exec address, accounting for the
    // split between code_exec and data_exec.
    auto vaddr_to_exec = [&](uint64_t vaddr) -> uint8_t* {
        uint64_t rel = vaddr - min_vaddr;
        if (using_data_jit && rel >= data_off_pg)
            return data_exec + (rel - data_off_pg);
        return code_exec + rel;
    };

    if (init_fn_vaddr && using_jit && this_svc_perm_code == 0) {
        so->init_fn = (LoadedSo::InitFn)vaddr_to_exec(init_fn_vaddr);
        compatLogFmt("ELF: DT_INIT fn deferred @%p", (void*)so->init_fn);
    }
    if (init_arr_vaddr && init_arr_sz && using_jit && this_svc_perm_code == 0) {
        so->init_arr       = (LoadedSo::InitFn*)vaddr_to_exec(init_arr_vaddr);
        so->init_arr_count = init_arr_sz / sizeof(LoadedSo::InitFn);
        compatLogFmt("ELF: %zu constructors deferred (arr=%p)",
                     so->init_arr_count, (void*)so->init_arr);
    }

    free(file_data);
    compatLogFmt("ELF: loaded OK code_exec=%p data_exec=%p sym_count=%u unresolved=%d",
                 (void*)code_exec, (void*)data_exec,
                 so->sym_count, g_unresolved_count);
    return so;
}
