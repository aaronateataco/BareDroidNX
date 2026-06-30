#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string>
#include "android.h"

// ELF64 types (no system elf.h in devkitA64)
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef uint64_t Elf64_Xword;
typedef int64_t  Elf64_Sxword;
typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;

typedef struct {
    unsigned char e_ident[16];
    Elf64_Half e_type, e_machine;
    Elf64_Word e_version;
    Elf64_Addr e_entry;
    Elf64_Off  e_phoff, e_shoff;
    Elf64_Word e_flags;
    Elf64_Half e_ehsize, e_phentsize, e_phnum;
    Elf64_Half e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    Elf64_Word  p_type, p_flags;
    Elf64_Off   p_offset;
    Elf64_Addr  p_vaddr, p_paddr;
    Elf64_Xword p_filesz, p_memsz, p_align;
} Elf64_Phdr;

typedef struct {
    Elf64_Word    st_name;
    unsigned char st_info, st_other;
    Elf64_Half    st_shndx;
    Elf64_Addr    st_value;
    Elf64_Xword   st_size;
} Elf64_Sym;

typedef struct {
    Elf64_Addr   r_offset;
    Elf64_Xword  r_info;
    Elf64_Sxword r_addend;
} Elf64_Rela;

typedef struct {
    Elf64_Sxword d_tag;
    union { Elf64_Xword d_val; Elf64_Addr d_ptr; } d_un;
} Elf64_Dyn;

#define ET_DYN      3
#define EM_AARCH64  183
#define PT_LOAD     1
#define PT_DYNAMIC  2
#define PF_X        0x1
#define PF_W        0x2
#define PF_R        0x4
#define DT_NULL     0
#define DT_NEEDED   1
#define DT_PLTRELSZ 2
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_RELA     7
#define DT_RELASZ   8
#define DT_SYMENT   11
#define DT_JMPREL   23
#define DT_PLTREL   20
#define DT_INIT_ARRAY    25
#define DT_FINI_ARRAY    26
#define DT_INIT_ARRAYSZ  27
#define DT_FINI_ARRAYSZ  28
#define DT_STRSZ    10
#define SHN_UNDEF   0
#define ELF64_R_SYM(i)  ((uint32_t)((i) >> 32))
#define ELF64_R_TYPE(i) ((uint32_t)((i) & 0xFFFFFFFFULL))
#define ELF64_ST_BIND(i) ((i) >> 4)
#define R_AARCH64_NONE      0
#define R_AARCH64_ABS64   257
#define R_AARCH64_COPY   1024
#define R_AARCH64_GLOB_DAT  1025
#define R_AARCH64_JUMP_SLOT 1026
#define R_AARCH64_RELATIVE  1027
#define ALIGN_UP(x,a)   (((uint64_t)(x) + (uint64_t)(a) - 1) & ~((uint64_t)(a) - 1))
#define ALIGN_DOWN(x,a) ((uint64_t)(x) & ~((uint64_t)(a) - 1))

// ─── LoadedSo ─────────────────────────────────────────────────────────────────
// One loaded ARM64 .so
struct LoadedSo {
    typedef void(*InitFn)();

    Jit          jit_mem;     // JIT memory handle (valid when using_jit is true)
    bool         using_jit = false;

    uint8_t*     alloc;       // exec (RX) allocation base pointer
    size_t       alloc_size;
    uint64_t     min_vaddr;   // first PT_LOAD p_vaddr
    uint8_t*     base;        // alloc - min_vaddr (exec side; base+vaddr = runtime ptr)

    // Heap copies of strtab/symtab so they survive jitTransitionToExecutable
    // (the JIT RW mapping is unmapped after the transition)
    char*        strtab_heap = nullptr;
    Elf64_Sym*   symtab_heap = nullptr;

    const char*  strtab;
    Elf64_Sym*   symtab;
    uint32_t     sym_count;
    uint64_t     strsz = 0;  // DT_STRSZ — bounds-checked in findSym against st_name

    // DT_INIT_ARRAY: stored here so constructors can be run after all SOs load
    InitFn*  init_arr       = nullptr;
    size_t   init_arr_count = 0;

    std::string  path;        // path on SD card

    // Find a symbol by name; returns runtime (exec) ptr or nullptr
    void* findSym(const char* name) const;
};

// ─── CompatLayer ─────────────────────────────────────────────────────────────
// High-level Android compatibility state
struct CompatLayer {
    ANativeActivity      activity;
    ANativeActivityCallbacks callbacks;
    ANativeWindow        window;
    AAssetManager        asset_mgr;
    ALooper              looper;
    AInputQueue          input_queue;

    // Fake JNI storage — owned by jni_env.cpp
    void*  vm_outer;   // passed to ANativeActivity.vm (JavaVM*)
    void*  env_outer;  // passed to ANativeActivity.env (JNIEnv*)
};

// ─── Launch result ────────────────────────────────────────────────────────────
struct LaunchResult {
    bool        ok          = false;
    std::string errorStage;   // which step failed (e.g. "Extracting APK")
    std::string errorDetail;  // human-readable reason
    int         unresolved  = 0;  // number of unresolved ELF symbols
    uint32_t    svcPermCode = 0;  // result of svcSetMemoryPermission (0 = OK)
};

// Progress callback invoked at each major launch stage.
// stage  = short label ("Extracting APK", "Loading ELF", …)
// detail = one-liner with more info (filename, size, etc.)
typedef void (*ProgressCb)(const char* stage, const char* detail);

// ─── Public API ───────────────────────────────────────────────────────────────
// Returns the global compat layer (singleton)
CompatLayer* compatGet();

// Load a single .so from disk — does NOT run constructors (call elfRunCtors after)
LoadedSo*    elfLoad(const char* path);
// Reset accumulated unresolved-symbol count and JIT error code (call before loading a batch)
void         elfResetCounts();
// Run a loaded SO's DT_INIT_ARRAY constructors (with per-entry logging and flush)
void         elfRunCtors(LoadedSo* so);
// Number of unresolved symbols accumulated since last elfResetCounts() call
int          elfGetUnresolvedCount();
// First JIT failure code since last elfResetCounts() call (0 = all OK)
uint32_t     elfGetLastSvcPermCode();
// Resolve a symbol across all loaded .so files, then shim table
void*        shimResolve(const char* name);
// Called from jni_env.cpp to install JNI/VM tables into compat layer
void         jniSetup(CompatLayer* cl);

// High-level launcher — extracts APK, loads libs, runs game.
// cb is called (if non-null) at each stage so the UI can show progress.
LaunchResult launchApk(const std::string& apk_path,
                       const std::string& pkg_name,
                       ProgressCb         cb = nullptr);
