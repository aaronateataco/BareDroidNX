#include "compat/loader.h"
#include "compat/android.h"
#include <switch.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>
#include <EGL/egl.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cerrno>
#include <ctime>
#include <cctype>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <malloc.h>
#include <strings.h>
#include <wchar.h>
#include <wctype.h>
#include <locale.h>
#include <setjmp.h>
#include <climits>
#include <fenv.h>
#include <poll.h>
#include <sys/socket.h>
#include <utime.h>

// Newlib stubs for POSIX functions that may be missing
static size_t stub_strnlen(const char* s, size_t n) {
    size_t i = 0;
    while (i < n && s[i]) i++;
    return i;
}
static char* stub_strtok_r(char* s, const char* d, char** save) {
    (void)save;
    return strtok(s, d);
}
static long stub_ftello(FILE* f) { return ftell(f); }
static int  stub_fseeko(FILE* f, long o, int w) { return fseek(f, o, w); }
static int  stub_setenv(const char*, const char*, int) { return 0; }
static int  stub_unsetenv(const char*)               { return 0; }
static int  stub_posix_memalign(void** p, size_t a, size_t s) {
    *p = memalign(a, s);
    return *p ? 0 : 12; // ENOMEM
}

// ─── New stubs for batch 3 (all symbols unresolved in the latest run) ─────────

// dladdr: crash reporters call this to resolve their own address → return failure
// Dl_info layout: 4 pointers (fname, fbase, sname, saddr) — zero them all
static int stub_dladdr(const void*, void* info) {
    if (info) memset(info, 0, 4 * sizeof(void*));
    return 0;
}
// sigaltstack: crash reporters use this to set up signal alt-stack → no-op
static int stub_sigaltstack(const void*, void*) { return 0; }
// signal: forward to newlib (returns SIG_DFL on failure)
// strsignal: return a static string
static char g_signame_buf[32];
static char* stub_strsignal(int sig) {
    snprintf(g_signame_buf, sizeof(g_signame_buf), "Signal %d", sig);
    return g_signame_buf;
}
// sys_signame: Android/BSD symbol — pointer to array of signal name strings
static const char* g_sys_signames[32] = {
    "", "HUP","INT","QUIT","ILL","TRAP","ABRT","BUS",
    "FPE","KILL","USR1","SEGV","USR2","PIPE","ALRM","TERM",
    "STKFLT","CHLD","CONT","STOP","TSTP","TTIN","TTOU","URG",
    "XCPU","XFSZ","VTALRM","PROF","WINCH","IO","PWR","SYS"
};
// environ: standard POSIX pointer to environment strings — expose newlib's
extern char** environ;
// gmtime_r / localtime_r — forward to newlib (may already exist, but explicit shim)
static struct tm* stub_gmtime_r(const time_t* t, struct tm* tm_) {
    struct tm* r = gmtime(t);
    if (r && tm_) { *tm_ = *r; return tm_; }
    return nullptr;
}
static struct tm* stub_localtime_r(const time_t* t, struct tm* tm_) {
    struct tm* r = localtime(t);
    if (r && tm_) { *tm_ = *r; return tm_; }
    return nullptr;
}
// __memset_chk / __strchr_chk — Bionic security wrappers
static void* stub_memset_chk(void* d, int c, size_t n, size_t /*dstlen*/) {
    return memset(d, c, n);
}
static char* stub_strchr_chk(const char* s, int c, size_t /*slen*/) {
    return (char*)strchr(s, c);
}
static int stub___FD_SET_chk(int fd, void* set, size_t /*setsize*/) {
    if (set && fd >= 0 && fd < 1024) { ((uint32_t*)set)[fd/32] |= (1u << (fd%32)); }
    return 0;
}
static int stub___FD_ISSET_chk(int fd, const void* set, size_t /*setsize*/) {
    if (!set || fd < 0 || fd >= 1024) return 0;
    return (((const uint32_t*)set)[fd/32] >> (fd%32)) & 1;
}
// Process stubs — Switch has no fork/exec/wait
static int stub_fork()                              { return -1; }
static int stub_execve(const char*, char* const*, char* const*) { errno = ENOSYS; return -1; }
static int stub_waitpid(int, int*, int)             { errno = ECHILD; return -1; }
static void stub__exit(int code)                    { exit(code); }
// Filesystem stubs missing from existing table
static int stub_symlink(const char*, const char*)   { errno = ENOSYS; return -1; }
static int stub_utimes(const char*, const void*)    { return 0; }
static char* stub_realpath(const char* p, char* out) {
    if (!out) {
        out = (char*)malloc(PATH_MAX);
        if (!out) return nullptr;
    }
    strncpy(out, p, PATH_MAX - 1); out[PATH_MAX-1] = '\0';
    return out;
}
static int stub_readlink(const char*, char* buf, size_t sz) {
    if (sz > 0 && buf) buf[0] = '\0';
    errno = EINVAL; return -1;
}
static int stub_chdir(const char* path) { return chdir(path); }
static int stub_isatty(int)             { return 0; }
// Network stubs
static int stub_setsockopt(int, int, int, const void*, unsigned) { errno = ENOTSUP; return -1; }
static int stub_accept(int, void*, void*)  { errno = ENOTSUP; return -1; }
static int stub_poll(void*, unsigned, int) { return 0; }
// User/group stubs
static int stub_setuid(unsigned) { return 0; }
static int stub_setgid(unsigned) { return 0; }
// popen/pclose stubs
static FILE* stub_popen(const char*, const char*) { return nullptr; }
static int   stub_pclose(FILE*)                   { return -1; }
// Terminal stubs
static int stub_tcgetattr(int, void*)         { errno = ENOTTY; return -1; }
static int stub_tcsetattr(int, int, const void*) { errno = ENOTTY; return -1; }
// malloc_usable_size — dlmalloc/newlib provides this
static size_t stub_malloc_usable_size(void* p) { return p ? malloc_usable_size(p) : 0; }
// Locale-variant char/string functions — ignore locale, call base version
static int stub_isdigit_l(int c, void*)   { return isdigit(c); }
static int stub_islower_l(int c, void*)   { return islower(c); }
static int stub_isupper_l(int c, void*)   { return isupper(c); }
static int stub_isxdigit_l(int c, void*)  { return isxdigit(c); }
static int stub_tolower_l(int c, void*)   { return tolower(c); }
static int stub_toupper_l(int c, void*)   { return toupper(c); }
static int stub_iswalpha_l(wint_t c, void*)   { return iswalpha(c); }
static int stub_iswblank_l(wint_t c, void*)   { return iswblank(c); }
static int stub_iswcntrl_l(wint_t c, void*)   { return iswcntrl(c); }
static int stub_iswdigit_l(wint_t c, void*)   { return iswdigit(c); }
static int stub_iswlower_l(wint_t c, void*)   { return iswlower(c); }
static int stub_iswprint_l(wint_t c, void*)   { return iswprint(c); }
static int stub_iswpunct_l(wint_t c, void*)   { return iswpunct(c); }
static int stub_iswspace_l(wint_t c, void*)   { return iswspace(c); }
static int stub_iswupper_l(wint_t c, void*)   { return iswupper(c); }
static int stub_iswxdigit_l(wint_t c, void*)  { return iswxdigit(c); }
static wint_t stub_towlower_l(wint_t c, void*) { return towlower(c); }
static wint_t stub_towupper_l(wint_t c, void*) { return towupper(c); }
static int stub_strcoll_l(const char* a, const char* b, void*) { return strcoll(a, b); }
static size_t stub_strxfrm_l(char* d, const char* s, size_t n, void*) { return strxfrm(d, s, n); }
static size_t stub_strftime_l(char* s, size_t m, const char* f, const struct tm* t, void*) {
    return strftime(s, m, f, t);
}
static int stub_wcscoll_l(const wchar_t* a, const wchar_t* b, void*) { return wcscoll(a, b); }
static size_t stub_wcsxfrm_l(wchar_t* d, const wchar_t* s, size_t n, void*) {
    return wcsxfrm(d, s, n);
}
// Math: forward to newlib (these exist but may be missing from our list)
static double stub_acosh(double x)  { return acosh(x); }
static double stub_asinh(double x)  { return asinh(x); }
static double stub_atanh(double x)  { return atanh(x); }
static double stub_log1p(double x)  { return log1p(x); }
static double stub_expm1(double x)  { return expm1(x); }
static double stub_difftime(time_t a, time_t b) { return difftime(a, b); }
// fesetround — forward to newlib
static int stub_fesetround(int r) { return fesetround(r); }
// strptime — newlib stub (may not exist in devkitA64 newlib)
static char* stub_strptime(const char*, const char*, struct tm*) { return nullptr; }
// clearerr / fileno / fdopen
static void  stub_clearerr(FILE* f)              { clearerr(f); }
static int   stub_fileno(FILE* f)                { return (f) ? (int)(size_t)f : -1; }
static FILE* stub_fdopen(int fd, const char* m)  { (void)fd; (void)m; return nullptr; }
// tmpfile — forward to newlib
static FILE* stub_tmpfile()                      { return tmpfile(); }

extern void compatLog(const char* msg);
extern void compatLogFmt(const char* fmt, ...);

// fopen wrapper — logs failed opens so we can see what paths game code requests
static FILE* stub_fopen(const char* path, const char* mode) {
    FILE* f = fopen(path, mode);
    if (!f) compatLogFmt("fopen FAIL: %s (mode=%s)", path ? path : "?", mode ? mode : "?");
    return f;
}
// open() wrapper — logs every call so we can trace early constructor I/O
static int stub_open(const char* path, int flags, ...) {
    int fd = open(path, flags);
    if (fd < 0) compatLogFmt("open FAIL: %s flags=0x%x", path ? path : "?", flags);
    else        compatLogFmt("open OK:   %s flags=0x%x fd=%d", path ? path : "?", flags, fd);
    return fd;
}

// ─── __android_log_print (liblog) ────────────────────────────────────────────
static int android_log_print(int, const char* tag, const char* fmt, ...) {
    char buf[512];
    va_list va;
    va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    compatLogFmt("[%s] %s", tag ? tag : "?", buf);
    return (int)strlen(buf);
}
static int android_log_write(int, const char* tag, const char* msg) {
    compatLogFmt("[%s] %s", tag ? tag : "?", msg ? msg : "");
    return 0;
}
static int android_log_vprint(int, const char* tag, const char* fmt, va_list va) {
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, va);
    compatLogFmt("[%s] %s", tag ? tag : "?", buf);
    return (int)strlen(buf);
}
static int android_log_buf_print(int, int, const char* tag, const char* fmt, ...) {
    va_list va; va_start(va, fmt);
    int r = android_log_vprint(0, tag, fmt, va);
    va_end(va); return r;
}

// ─── pthread stubs ───────────────────────────────────────────────────────────
// Switch (libnx) doesn't expose pthreads.  Single-threaded stubs.
static void* g_pthread_tls[64] = {};
static int   g_tls_key_count   = 0;

static int pt_mutex_init(void* m, const void*)  { memset(m, 0, 40); return 0; }
static int pt_mutex_lock(void*)                  { return 0; }
static int pt_mutex_unlock(void*)                { return 0; }
static int pt_mutex_trylock(void*)               { return 0; }
static int pt_mutex_destroy(void*)               { return 0; }
static int pt_cond_init(void* c, const void*)    { memset(c, 0, 48); return 0; }
static int pt_cond_signal(void*)                 { return 0; }
static int pt_cond_broadcast(void*)              { return 0; }
static int pt_cond_wait(void*, void*)            { return 0; }
static int pt_cond_timedwait(void*, void*, const void*) { return 0; }
static int pt_cond_destroy(void*)                { return 0; }
static int pt_rwlock_init(void* l, const void*)  { memset(l, 0, 56); return 0; }
static int pt_rwlock_rdlock(void*)               { return 0; }
static int pt_rwlock_wrlock(void*)               { return 0; }
static int pt_rwlock_unlock(void*)               { return 0; }
static int pt_rwlock_destroy(void*)              { return 0; }
static int pt_create(void** t, const void*, void* (*fn)(void*), void* arg) {
    // We can't create real threads, so we log and return a fake handle.
    // This will break threaded games, but prevents the crash.
    compatLog("WARN: pthread_create called — threads not supported, ignoring");
    *t = (void*)0x1;
    (void)fn; (void)arg;
    return 0;
}
static int pt_join(void*, void** ret)   { if (ret) *ret = nullptr; return 0; }
static int pt_detach(void*)             { return 0; }
static void* pt_self(void)             { return (void*)0x1; }
static int pt_equal(void* a, void* b)  { return a == b ? 1 : 0; }
static int pt_key_create(int* k, void (*)(void*)) {
    if (g_tls_key_count >= 64) return 11; // EAGAIN
    *k = g_tls_key_count++;
    return 0;
}
static int pt_key_delete(int) { return 0; }
static void* pt_getspecific(int k) {
    return (k >= 0 && k < 64) ? g_pthread_tls[k] : nullptr;
}
static int pt_setspecific(int k, const void* v) {
    if (k < 0 || k >= 64) return 22; // EINVAL
    g_pthread_tls[k] = (void*)v;
    return 0;
}
static int pt_once(int* ctrl, void (*fn)(void)) {
    if (*ctrl == 0) {
        compatLogFmt("pthread_once: calling fn @%p", (void*)fn);
        *ctrl = 1;
        fn();
        compatLog("pthread_once: fn returned");
    }
    return 0;
}
static int pt_attr_init(void* a)    { memset(a, 0, 56); return 0; }
static int pt_attr_destroy(void*)   { return 0; }
static int pt_attr_setdetachstate(void*, int) { return 0; }
static int pt_attr_setstacksize(void*, size_t){ return 0; }
static int pt_attr_getstacksize(const void*, size_t* s) { if (s) *s = 65536; return 0; }

// ─── errno access (Bionic uses __errno() function) ───────────────────────────
static int* bionic_errno(void) { return &errno; }

// ─── dlopen / dlsym stubs ────────────────────────────────────────────────────
static void* fake_dlopen(const char* path, int) {
    compatLogFmt("dlopen: %s", path ? path : "(null)");
    return (void*)0xDEAD;  // non-null means "success"
}
static void* fake_dlsym(void*, const char* sym) {
    // Recurse into our own shim table
    void* p = shimResolve(sym);
    if (!p) compatLogFmt("dlsym: unresolved %s", sym ? sym : "?");
    return p;
}
static int fake_dlclose(void*) { return 0; }
static const char* fake_dlerror(void) { return nullptr; }

// ─── libandroid shims ────────────────────────────────────────────────────────
// AAssetManager
static AAsset* asset_open(AAssetManager* mgr, const char* fn, int) {
    if (!mgr || !fn) return nullptr;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", mgr->base_path, fn);
    FILE* f = fopen(path, "rb");
    if (!f) {
        compatLogFmt("AAsset_open: not found: %s", path);
        return nullptr;
    }
    AAsset* a = (AAsset*)calloc(1, sizeof(AAsset));
    a->fp = f;
    fseek(f, 0, SEEK_END);
    a->size = (int64_t)ftell(f);
    rewind(f);
    strncpy(a->path, path, sizeof(a->path) - 1);
    return a;
}
static void   asset_close(AAsset* a)  { if (a) { fclose(a->fp); free(a); } }
static int    asset_read(AAsset* a, void* buf, size_t n) {
    if (!a) return -1;
    return (int)fread(buf, 1, n, a->fp);
}
static int64_t asset_seek(AAsset* a, int64_t off, int whence) {
    if (!a) return -1;
    fseek(a->fp, (long)off, whence);
    return (int64_t)ftell(a->fp);
}
static int64_t asset_seek64(AAsset* a, int64_t off, int whence) {
    return asset_seek(a, off, whence);
}
static int64_t asset_length(AAsset* a)         { return a ? a->size : 0; }
static int64_t asset_remain(AAsset* a) {
    if (!a) return 0;
    return a->size - (int64_t)ftell(a->fp);
}
static const void* asset_buffer(AAsset*) { return nullptr; }  // no mmap on Switch
static int asset_isAllocated(AAsset*)    { return 0; }
static AAssetManager* assetMgr_fromJava(void*) {
    // Return the global compat layer's asset manager
    return &compatGet()->asset_mgr;
}
static AAssetDir* asset_openDir(AAssetManager*, const char* d) {
    (void)d; return (AAssetDir*)0x1; // stub
}
static const char* asset_dirNext(AAssetDir*) { return nullptr; }
static void        asset_dirRewind(AAssetDir*) {}
static void        asset_dirClose(AAssetDir*) {}

// ANativeWindow
static int32_t nwin_getWidth(ANativeWindow* w)  { return w ? w->width  : 1280; }
static int32_t nwin_getHeight(ANativeWindow* w) { return w ? w->height : 720;  }
static int32_t nwin_getFormat(ANativeWindow* w) { return w ? w->format : 1; }
static int32_t nwin_setBuffersGeometry(ANativeWindow* w, int32_t wd, int32_t ht, int32_t fmt) {
    if (w) { w->width = wd; w->height = ht; w->format = fmt; }
    return 0;
}
static void nwin_acquire(ANativeWindow*) {}
static void nwin_release(ANativeWindow*) {}
static int  nwin_lock(ANativeWindow*, void* buf, const void* dirtyBounds) {
    (void)buf; (void)dirtyBounds; return -1;
}
static int  nwin_unlockAndPost(ANativeWindow*) { return -1; }

// ALooper
static ALooper* looper_forThread(void) { return &compatGet()->looper; }
static ALooper* looper_prepare(int)    { return &compatGet()->looper; }
static void     looper_acquire(ALooper*) {}
static void     looper_release(ALooper*) {}
static int looper_pollOnce(int timeout, int* fd, int* events, void** data) {
    (void)timeout; (void)fd; (void)events; (void)data;
    return ALOOPER_POLL_TIMEOUT;
}
static int looper_pollAll(int timeout, int* fd, int* events, void** data) {
    return looper_pollOnce(timeout, fd, events, data);
}
static void looper_wake(ALooper*) {}
static int  looper_addFd(ALooper*, int, int, int, void*, void*) { return 1; }
static int  looper_removeFd(ALooper*, int) { return 1; }

// AInputEvent
static int32_t  ev_getType(const AInputEvent* e) { return e ? e->type   : 0; }
static int32_t  ev_getAction(const AInputEvent* e){ return e ? e->action : 0; }
static float    ev_getX(const AInputEvent* e, size_t) { return e ? e->x  : 0; }
static float    ev_getY(const AInputEvent* e, size_t) { return e ? e->y  : 0; }
static int32_t  ev_getKeyCode(const AInputEvent*)  { return 0; }
static int32_t  ev_getMetaState(const AInputEvent*){ return 0; }
static int32_t  ev_getSource(const AInputEvent*)   { return 0; }
static size_t   ev_getPointerCount(const AInputEvent*){ return 1; }
static float    ev_getPressure(const AInputEvent*, size_t) { return 1.0f; }
static float    ev_getSize(const AInputEvent*, size_t) { return 0.0f; }
static int32_t  ev_getPointerId(const AInputEvent*, size_t idx) { return (int32_t)idx; }
static void     ev_setAction(AInputEvent*, int)    {}

static int iq_getEvent(AInputQueue*, AInputEvent** e) { (void)e; return -1; }
static int iq_hasEvents(AInputQueue*) { return 0; }
static void iq_finishEvent(AInputQueue*, AInputEvent*, int) {}

// Configuration (AConfiguration — minimal stubs)
static void* acfg_new(void)    { return calloc(1, 256); }
static void  acfg_delete(void* c) { free(c); }
static void  acfg_fromAssetManager(void*, AAssetManager*) {}
static int32_t acfg_getDensity(void*) { return 320; } // xhdpi (Switch screen)
static int32_t acfg_getOrientation(void*) { return 2; } // landscape
static int32_t acfg_getScreenSize(void*) { return 3; } // large
static int32_t acfg_getSdkVersion(void*) { return 26; }

// ─── sincosf ─────────────────────────────────────────────────────────────────
static void stub_sincosf(float x, float* s, float* c) { *s = sinf(x); *c = cosf(x); }

// ─── stpcpy (POSIX — may not be exposed by newlib header) ────────────────────
static char* stub_stpcpy(char* dst, const char* src) {
    size_t n = strlen(src);
    memcpy(dst, src, n + 1);
    return dst + n;
}

// ─── vasprintf (GNU extension) ────────────────────────────────────────────────
static int stub_vasprintf(char** out, const char* fmt, va_list va) {
    va_list va2; va_copy(va2, va);
    int n = vsnprintf(nullptr, 0, fmt, va2); va_end(va2);
    if (n < 0) { *out = nullptr; return -1; }
    *out = (char*)malloc((size_t)n + 1);
    if (!*out) return -1;
    vsprintf(*out, fmt, va);
    return n;
}

// ─── POSIX semaphores (single-threaded stubs) ─────────────────────────────────
struct BnxSem { volatile int value; };
static int stub_sem_init(BnxSem* s, int, unsigned int v) { s->value = (int)v; return 0; }
static int stub_sem_destroy(BnxSem*) { return 0; }
static int stub_sem_post(BnxSem* s) { s->value++; return 0; }
static int stub_sem_wait(BnxSem* s) {
    if (s->value > 0) { s->value--; return 0; }
    compatLog("WARN: sem_wait on empty semaphore — pretending it's OK");
    return 0;
}
static int stub_sem_trywait(BnxSem* s) {
    if (s->value > 0) { s->value--; return 0; }
    errno = EAGAIN; return -1;
}

// ─── Network stubs (no BSD socket service configured) ────────────────────────
static int stub_socket(int, int, int)               { errno = ENOTSUP; return -1; }
static int stub_bind(int, const void*, unsigned)    { errno = ENOTSUP; return -1; }
static int stub_connect(int, const void*, unsigned) { errno = ECONNREFUSED; return -1; }
static int stub_listen(int, int)                    { errno = ENOTSUP; return -1; }
static int stub_select(int, void*, void*, void*, void*) { errno = ENOTSUP; return -1; }
static ssize_t stub_recv(int, void*, size_t, int)   { errno = ENOTSUP; return -1; }
static ssize_t stub_send(int, const void*, size_t, int) { errno = ENOTSUP; return -1; }
static int stub_getsockname(int, void*, unsigned*)  { errno = ENOTSUP; return -1; }
static int stub_getsockopt(int, int, int, void*, unsigned*) { errno = ENOTSUP; return -1; }
static void* stub_gethostbyname(const char*)        { return nullptr; }
static int stub_fcntl_sock(int, int, ...)           { return 0; }
static int* stub_get_h_errno(void)                  { static int h = 0; return &h; }

// ─── Scheduling ──────────────────────────────────────────────────────────────
static int stub_sched_yield(void) { svcSleepThread(0); return 0; }

// ─── System ──────────────────────────────────────────────────────────────────
static long stub_sysconf(int n) {
    switch (n) {
        case 30: return 4096;  // _SC_PAGESIZE
        case 84: return 4;     // _SC_NPROCESSORS_ONLN — Tegra X1 has 4 ARM cores
        default:
            compatLogFmt("sysconf(%d) -> -1 (unknown)", n);
            return -1;
    }
}
static char* stub_getcwd(char* buf, size_t sz) {
    if (!buf || sz < 2) return nullptr;
    buf[0] = '/'; buf[1] = '\0'; return buf;
}
static long stub_syscall(long, ...) { errno = ENOSYS; return -1; }
static int  stub_dl_iterate_phdr(void*, void*) { return 0; }

// ─── Android-specific ────────────────────────────────────────────────────────
static void stub_android_abort_msg(const char* msg) {
    compatLogFmt("android_abort_message: %s", msg ? msg : "");
}

// ─── syslog ──────────────────────────────────────────────────────────────────
static void stub_openlog(const char* id, int, int) {
    compatLogFmt("openlog: %s", id ? id : "");
}
static void stub_closelog(void) {}
static void stub_syslog(int, const char* fmt, ...) {
    char buf[512]; va_list va; va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va); va_end(va);
    compatLog(buf);
}

// ─── Locale POSIX extensions (stub — newlib may lack these) ──────────────────
typedef void* bnx_locale_t;
static bnx_locale_t stub_newlocale(int, const char*, bnx_locale_t) { return (bnx_locale_t)1; }
static void stub_freelocale(bnx_locale_t) {}
static bnx_locale_t stub_uselocale(bnx_locale_t) { return (bnx_locale_t)1; }
static int stub_mb_cur_max(void) { return 1; }

// ─── strtod/strtoll locale variants ─────────────────────────────────────────
static long long        stub_strtoll_l(const char* s, char** e, int b, void*) { return strtoll(s, e, b); }
static unsigned long long stub_strtoull_l(const char* s, char** e, int b, void*) { return strtoull(s, e, b); }
static long double      stub_strtold_l(const char* s, char** e, void*)        { return strtold(s, e); }

// ─── Bionic fortified wrappers ────────────────────────────────────────────────
static size_t  chk_strlen(const char* s, size_t)               { return strlen(s); }
static void*   chk_memcpy(void* d, const void* s, size_t n, size_t)   { return memcpy(d,s,n); }
static void*   chk_memmove(void* d, const void* s, size_t n, size_t)  { return memmove(d,s,n); }
static char*   chk_strcat(char* d, const char* s, size_t)     { return strcat(d,s); }
static char*   chk_strcpy(char* d, const char* s, size_t)     { return strcpy(d,s); }
static char*   chk_strncpy(char* d, const char* s, size_t n, size_t, size_t) { return strncpy(d,s,n); }
static int     chk_vsprintf(char* d, int, size_t, const char* f, va_list v)  { return vsprintf(d,f,v); }
static int     chk_vsnprintf(char* d, size_t n, int, size_t, const char* f, va_list v) { return vsnprintf(d,n,f,v); }
static ssize_t chk_read(int fd, void* b, size_t n, size_t)    { return read(fd,b,n); }
static int     chk_open2(const char* p, int fl)                { return open(p, fl, 0666); }

// ─── pthread_mutexattr extras ─────────────────────────────────────────────────
static int pt_mattr_init(void* a)       { if (a) memset(a, 0, 4); return 0; }
static int pt_mattr_destroy(void*)      { return 0; }
static int pt_mattr_settype(void*, int) { return 0; }

// ─── prctl (Linux process/thread control — crash reporters use PR_SET_NAME) ───
static int stub_prctl(int, unsigned long, unsigned long, unsigned long, unsigned long) {
    return 0;
}

// ─── gettid (Linux syscall — thread ID, not in newlib) ──────────────────────
static pid_t stub_gettid(void) { return 1; }

// ─── getpid / getuid / getgid ────────────────────────────────────────────────
// newlib has getpid() but shimming it makes it available to the game's .so
static pid_t stub_getpid(void)  { return 1; }
static uid_t stub_getuid(void)  { return 0; }
static gid_t stub_getgid(void)  { return 0; }

// ─── Signal stubs (crash reporters install SIGSEGV/SIGBUS handlers) ─────────
// Return success without actually doing anything — Switch uses its own fault handler
struct BnxSigaction { void* handler; unsigned long flags; void* restorer; uint64_t mask; };
static int stub_sigaction(int, const BnxSigaction*, BnxSigaction*) { return 0; }
static int stub_sigemptyset(uint64_t* s) { if (s) *s = 0; return 0; }
static int stub_sigfillset(uint64_t* s)  { if (s) *s = ~(uint64_t)0; return 0; }
static int stub_sigaddset(uint64_t* s, int sig) {
    if (s && sig > 0 && sig < 64) *s |= (1ULL << (sig - 1));
    return 0;
}
static int stub_sigdelset(uint64_t* s, int sig) {
    if (s && sig > 0 && sig < 64) *s &= ~(1ULL << (sig - 1));
    return 0;
}
static int stub_sigismember(const uint64_t* s, int sig) {
    if (!s || sig <= 0 || sig >= 64) return 0;
    return (*s >> (sig - 1)) & 1;
}
static int stub_kill(pid_t, int)    { return 0; }
static int stub_raise(int)          { return 0; }  // prevent crash reporter self-test
static int stub_pthread_kill(void*, int) { return 0; }
static int stub_sigprocmask(int, const uint64_t*, uint64_t*) { return 0; }
static int stub_pthread_sigmask(int, const uint64_t*, uint64_t*) { return 0; }

// ─── mprotect stub (crash reporters may set guard-page permissions) ──────────
static int stub_mprotect(void*, size_t, int) { return 0; }

// ─── pipe / dup / dup2 (crash reporter IPC) ─────────────────────────────────
static int stub_pipe(int fd[2])          { (void)fd; errno = ENOTSUP; return -1; }
static int stub_dup(int)                 { errno = ENOTSUP; return -1; }
static int stub_dup2(int, int)           { errno = ENOTSUP; return -1; }

// ─── ioctl stub ──────────────────────────────────────────────────────────────
static int stub_ioctl(int, unsigned long, void*) { errno = ENOTSUP; return -1; }

// ─── access stub (file existence check) ──────────────────────────────────────
static int stub_access(const char*, int) { errno = ENOENT; return -1; }

// ─── chmod / fchmod / lstat ──────────────────────────────────────────────────
static int stub_chmod(const char*, mode_t)   { return 0; }
static int stub_fchmod(int, mode_t)          { return 0; }
static int stub_lstat(const char* p, struct stat* s) { return stat(p, s); }

// ─── pthread_setname_np / pthread_getname_np (thread naming, GNU ext) ────────
static int stub_pthread_setname_np(void*, const char*) { return 0; }
static int stub_pthread_getname_np(void*, char* buf, size_t sz) {
    if (buf && sz > 0) buf[0] = '\0'; return 0;
}
static int stub_pthread_attr_setstack(void*, void*, size_t) { return 0; }
static int stub_pthread_attr_getstack(const void*, void** s, size_t* z) {
    if (s) *s = nullptr; if (z) *z = 65536; return 0;
}
static int stub_pthread_attr_setschedpolicy(void*, int) { return 0; }
static int stub_pthread_attr_setschedparam(void*, const void*) { return 0; }
static int stub_pthread_attr_getschedparam(const void*, void*) { return 0; }
static int stub_pthread_barrier_init(void*, const void*, unsigned) { return 0; }
static int stub_pthread_barrier_wait(void*) { return 0; }
static int stub_pthread_barrier_destroy(void*) { return 0; }

// ─── getauxval (Android uses AT_HWCAP for NEON detection) ───────────────────
static unsigned long stub_getauxval(unsigned long type) {
    switch (type) {
        case 16: return 0x1001;   // AT_HWCAP: NEON (bit 12) + basic ARM64
        case 26: return 0;        // AT_HWCAP2
        default: return 0;
    }
}

// ─── sleep / usleep (common in init code) ────────────────────────────────────
static unsigned int stub_sleep(unsigned int sec) {
    svcSleepThread((uint64_t)sec * 1000000000ULL); return 0;
}
static int stub_usleep(unsigned int usec) {
    svcSleepThread((uint64_t)usec * 1000ULL); return 0;
}

// ─── clock_nanosleep ──────────────────────────────────────────────────────────
static int stub_clock_nanosleep(int, int, const struct timespec* req, struct timespec*) {
    if (req) svcSleepThread(req->tv_sec * 1000000000LL + req->tv_nsec);
    return 0;
}

// ─── strtod_l / strtof_l locale variants ────────────────────────────────────
static double      stub_strtod_l(const char* s, char** e, void*) { return strtod(s, e); }
static float       stub_strtof_l(const char* s, char** e, void*) { return strtof(s, e); }

// ─── mmap / munmap (crash reporters may use anon mmap for stack unwinding) ───
// Map via memalign as a best-effort fallback; executable mapping not supported.
static void* stub_mmap(void*, size_t len, int, int, int, long) {
    void* p = memalign(0x1000, len);
    if (!p) { errno = ENOMEM; return (void*)(uintptr_t)-1; }
    memset(p, 0, len);
    return p;
}
static int stub_munmap(void* p, size_t) { free(p); return 0; }

// ─── __register_atfork (pthread fork support — no-op on Switch) ─────────────
static int stub_register_atfork(void*, void*, void*, void*) { return 0; }

// ─── wcsnrtombs / mbsnrtowcs (GNU ext — stub in case newlib lacks them) ──────
static size_t stub_wcsnrtombs(char* d, const wchar_t** src, size_t nwc, size_t len, void*) {
    if (!src || !*src || !len) return 0;
    size_t w = 0;
    while (nwc-- && **src) {
        char mb[MB_LEN_MAX];
        int n = wctomb(mb, *(*src)++);
        if (n <= 0 || w + (size_t)n >= len) break;
        if (d) memcpy(d + w, mb, n);
        w += n;
    }
    if (d && w < len) d[w] = '\0';
    return w;
}
static size_t stub_mbsnrtowcs(wchar_t* d, const char** src, size_t nmc, size_t len, void*) {
    if (!src || !*src) return 0;
    size_t count = 0;
    while (nmc > 0 && **src && count < len) {
        wchar_t wc;
        int n = mbtowc(&wc, *src, nmc);
        if (n <= 0) break;
        if (d) d[count] = wc;
        count++; *src += n; nmc -= (size_t)n;
    }
    return count;
}

// ─── __sF (Bionic stdio backing array for stdin/stdout/stderr) ───────────────
// Bionic defines FILE __sF[3]; our games may reference &__sF[N] as a FILE*.
// Provide a zero-filled placeholder so GOT entries are non-null.
static uint8_t g_fake_sF[3 * 256] = {};

// ─── Stack protection (Bionic provides these; stub for newlib) ────────────────
extern "C" { uintptr_t __stack_chk_guard = 0xDEAD0BEEF; }
extern "C" void __stack_chk_fail(void) {
    compatLog("FATAL: stack smash detected");
    abort();
}
extern "C" void __cxa_pure_virtual(void) { abort(); }
extern "C" void __cxa_atexit(void*, void*, void*) {}

// ─── Unwind stubs ────────────────────────────────────────────────────────────
// _Unwind_* are already in libgcc; just declare externs so we can take their address.
// __gnu_unwind_frame is ARM-specific; define a stub only if libgcc doesn't have it.
extern "C" {
    void _Unwind_Resume(void*);
    void* _Unwind_GetLanguageSpecificData(void*);
    uintptr_t _Unwind_GetIP(void*);
    void _Unwind_SetIP(void*, uintptr_t);
    uintptr_t _Unwind_GetRegionStart(void*);
}
static void stub_gnu_unwind_frame(void*, void*) {}

// ─── Shim table ──────────────────────────────────────────────────────────────
struct ShimEntry { const char* name; void* ptr; };

static const ShimEntry g_shims[] = {
    // ── liblog ──────────────────────────────────────────────────────────────
    {"__android_log_print",     (void*)android_log_print},
    {"__android_log_write",     (void*)android_log_write},
    {"__android_log_vprint",    (void*)android_log_vprint},
    {"__android_log_buf_print", (void*)android_log_buf_print},

    // ── libc / newlib passthrough ────────────────────────────────────────────
    {"malloc",      (void*)malloc},
    {"free",        (void*)free},
    {"calloc",      (void*)calloc},
    {"realloc",     (void*)realloc},
    {"memalign",    (void*)memalign},
    {"posix_memalign",(void*)stub_posix_memalign},
    {"memcpy",      (void*)memcpy},
    {"memmove",     (void*)memmove},
    {"memset",      (void*)memset},
    {"memcmp",      (void*)memcmp},
    {"memchr",      (void*)memchr},
    {"strlen",      (void*)strlen},
    {"strnlen",     (void*)stub_strnlen},
    {"strcmp",      (void*)strcmp},
    {"strncmp",     (void*)strncmp},
    {"strcasecmp",  (void*)strcasecmp},
    {"strncasecmp", (void*)strncasecmp},
    {"strcpy",      (void*)strcpy},
    {"strncpy",     (void*)strncpy},
    {"strcat",      (void*)strcat},
    {"strncat",     (void*)strncat},
    {"strchr",      (void*)strchr},
    {"strrchr",     (void*)strrchr},
    {"strstr",      (void*)strstr},
    {"strtok",      (void*)strtok},
    {"strtok_r",    (void*)stub_strtok_r},
    {"strtol",      (void*)strtol},
    {"strtoul",     (void*)strtoul},
    {"strtoll",     (void*)strtoll},
    {"strtoull",    (void*)strtoull},
    {"strtof",      (void*)strtof},
    {"strtod",      (void*)strtod},
    {"atoi",        (void*)atoi},
    {"atol",        (void*)atol},
    {"atoll",       (void*)atoll},
    {"atof",        (void*)atof},
    {"sprintf",     (void*)sprintf},
    {"snprintf",    (void*)snprintf},
    {"sscanf",      (void*)sscanf},
    {"printf",      (void*)printf},
    {"fprintf",     (void*)fprintf},
    {"vprintf",     (void*)vprintf},
    {"vfprintf",    (void*)vfprintf},
    {"vsprintf",    (void*)vsprintf},
    {"vsnprintf",   (void*)vsnprintf},
    {"fopen",       (void*)stub_fopen},
    {"fclose",      (void*)fclose},
    {"fread",       (void*)fread},
    {"fwrite",      (void*)fwrite},
    {"fseek",       (void*)fseek},
    {"ftell",       (void*)ftell},
    {"fseeko",      (void*)stub_fseeko},
    {"ftello",      (void*)stub_ftello},
    {"rewind",      (void*)rewind},
    {"fflush",      (void*)fflush},
    {"feof",        (void*)feof},
    {"ferror",      (void*)ferror},
    {"fgets",       (void*)fgets},
    {"fputs",       (void*)fputs},
    {"fgetc",       (void*)fgetc},
    {"fputc",       (void*)fputc},
    {"getc",        (void*)getc},
    {"putc",        (void*)putc},
    {"ungetc",      (void*)ungetc},
    {"open",        (void*)stub_open},
    {"close",       (void*)close},
    {"read",        (void*)read},
    {"write",       (void*)write},
    {"lseek",       (void*)lseek},
    {"stat",        (void*)stat},
    {"fstat",       (void*)fstat},
    {"mkdir",       (void*)mkdir},
    {"opendir",     (void*)opendir},
    {"readdir",     (void*)readdir},
    {"closedir",    (void*)closedir},
    {"abort",       (void*)abort},
    {"exit",        (void*)exit},
    {"qsort",       (void*)qsort},
    {"bsearch",     (void*)bsearch},
    {"rand",        (void*)rand},
    {"srand",       (void*)srand},
    {"time",        (void*)time},
    {"clock",       (void*)clock},
    {"getenv",      (void*)getenv},
    {"setenv",      (void*)stub_setenv},
    {"unsetenv",    (void*)stub_unsetenv},
    {"__errno",     (void*)bionic_errno},
    {"__stack_chk_fail",   (void*)__stack_chk_fail},
    {"__cxa_atexit",       (void*)__cxa_atexit},
    {"__cxa_pure_virtual", (void*)__cxa_pure_virtual},

    // ── libm passthrough ─────────────────────────────────────────────────────
    {"sin",   (void*)sin},   {"sinf",  (void*)sinf},
    {"cos",   (void*)cos},   {"cosf",  (void*)cosf},
    {"tan",   (void*)tan},   {"tanf",  (void*)tanf},
    {"asin",  (void*)asin},  {"asinf", (void*)asinf},
    {"acos",  (void*)acos},  {"acosf", (void*)acosf},
    {"atan",  (void*)atan},  {"atanf", (void*)atanf},
    {"atan2", (void*)atan2}, {"atan2f",(void*)atan2f},
    {"sqrt",  (void*)sqrt},  {"sqrtf", (void*)sqrtf},
    {"pow",   (void*)pow},   {"powf",  (void*)powf},
    {"exp",   (void*)exp},   {"expf",  (void*)expf},
    {"exp2",  (void*)exp2},  {"exp2f", (void*)exp2f},
    {"log",   (void*)log},   {"logf",  (void*)logf},
    {"log2",  (void*)log2},  {"log2f", (void*)log2f},
    {"log10", (void*)log10}, {"log10f",(void*)log10f},
    {"floor", (void*)floor}, {"floorf",(void*)floorf},
    {"ceil",  (void*)ceil},  {"ceilf", (void*)ceilf},
    {"round", (void*)round}, {"roundf",(void*)roundf},
    {"fabs",  (void*)fabs},  {"fabsf", (void*)fabsf},
    {"fmod",  (void*)fmod},  {"fmodf", (void*)fmodf},
    {"fmin",  (void*)fmin},  {"fminf", (void*)fminf},
    {"fmax",  (void*)fmax},  {"fmaxf", (void*)fmaxf},
    {"hypot", (void*)hypot}, {"hypotf",(void*)hypotf},
    {"ldexp", (void*)ldexp}, {"ldexpf",(void*)ldexpf},
    {"frexp", (void*)frexp}, {"frexpf",(void*)frexpf},
    {"modf",  (void*)modf},  {"modff", (void*)modff},
    {"trunc", (void*)trunc}, {"truncf",(void*)truncf},
    {"copysign",(void*)copysign}, {"copysignf",(void*)copysignf},
    {"cbrt",  (void*)cbrt},  {"cbrtf", (void*)cbrtf},
    {"sinh",  (void*)sinh},  {"sinhf", (void*)sinhf},
    {"cosh",  (void*)cosh},  {"coshf", (void*)coshf},
    {"tanh",  (void*)tanh},  {"tanhf", (void*)tanhf},
    // isinf/isnan are macros in C99; provide lambda-wrapped stubs
    {"isinf", (void*)+[](double x) -> int { return std::isinf(x); }},
    {"isnan", (void*)+[](double x) -> int { return std::isnan(x); }},

    // ── pthread stubs ────────────────────────────────────────────────────────
    {"pthread_mutex_init",     (void*)pt_mutex_init},
    {"pthread_mutex_lock",     (void*)pt_mutex_lock},
    {"pthread_mutex_unlock",   (void*)pt_mutex_unlock},
    {"pthread_mutex_trylock",  (void*)pt_mutex_trylock},
    {"pthread_mutex_destroy",  (void*)pt_mutex_destroy},
    {"pthread_cond_init",      (void*)pt_cond_init},
    {"pthread_cond_signal",    (void*)pt_cond_signal},
    {"pthread_cond_broadcast", (void*)pt_cond_broadcast},
    {"pthread_cond_wait",      (void*)pt_cond_wait},
    {"pthread_cond_timedwait", (void*)pt_cond_timedwait},
    {"pthread_cond_destroy",   (void*)pt_cond_destroy},
    {"pthread_rwlock_init",    (void*)pt_rwlock_init},
    {"pthread_rwlock_rdlock",  (void*)pt_rwlock_rdlock},
    {"pthread_rwlock_wrlock",  (void*)pt_rwlock_wrlock},
    {"pthread_rwlock_unlock",  (void*)pt_rwlock_unlock},
    {"pthread_rwlock_destroy", (void*)pt_rwlock_destroy},
    {"pthread_create",         (void*)pt_create},
    {"pthread_join",           (void*)pt_join},
    {"pthread_detach",         (void*)pt_detach},
    {"pthread_self",           (void*)pt_self},
    {"pthread_equal",          (void*)pt_equal},
    {"pthread_key_create",     (void*)pt_key_create},
    {"pthread_key_delete",     (void*)pt_key_delete},
    {"pthread_getspecific",    (void*)pt_getspecific},
    {"pthread_setspecific",    (void*)pt_setspecific},
    {"pthread_once",           (void*)pt_once},
    {"pthread_attr_init",       (void*)pt_attr_init},
    {"pthread_attr_destroy",    (void*)pt_attr_destroy},
    {"pthread_attr_setdetachstate",(void*)pt_attr_setdetachstate},
    {"pthread_attr_setstacksize", (void*)pt_attr_setstacksize},
    {"pthread_attr_getstacksize", (void*)pt_attr_getstacksize},

    // ── libdl ────────────────────────────────────────────────────────────────
    {"dlopen",  (void*)fake_dlopen},
    {"dlsym",   (void*)fake_dlsym},
    {"dlclose", (void*)fake_dlclose},
    {"dlerror", (void*)fake_dlerror},

    // ── libandroid ───────────────────────────────────────────────────────────
    {"AAssetManager_fromJava",      (void*)assetMgr_fromJava},
    {"AAssetManager_open",          (void*)asset_open},
    {"AAssetManager_openDir",       (void*)asset_openDir},
    {"AAssetDir_getNextFileName",   (void*)asset_dirNext},
    {"AAssetDir_rewind",            (void*)asset_dirRewind},
    {"AAssetDir_close",             (void*)asset_dirClose},
    {"AAsset_close",                (void*)asset_close},
    {"AAsset_read",                 (void*)asset_read},
    {"AAsset_seek",                 (void*)asset_seek},
    {"AAsset_seek64",               (void*)asset_seek64},
    {"AAsset_getLength",            (void*)asset_length},
    {"AAsset_getLength64",          (void*)asset_length},
    {"AAsset_getRemainingLength",   (void*)asset_remain},
    {"AAsset_getRemainingLength64", (void*)asset_remain},
    {"AAsset_getBuffer",            (void*)asset_buffer},
    {"AAsset_isAllocated",          (void*)asset_isAllocated},
    {"ANativeWindow_getWidth",      (void*)nwin_getWidth},
    {"ANativeWindow_getHeight",     (void*)nwin_getHeight},
    {"ANativeWindow_getFormat",     (void*)nwin_getFormat},
    {"ANativeWindow_setBuffersGeometry", (void*)nwin_setBuffersGeometry},
    {"ANativeWindow_acquire",       (void*)nwin_acquire},
    {"ANativeWindow_release",       (void*)nwin_release},
    {"ANativeWindow_lock",          (void*)nwin_lock},
    {"ANativeWindow_unlockAndPost", (void*)nwin_unlockAndPost},
    {"ALooper_forThread",           (void*)looper_forThread},
    {"ALooper_prepare",             (void*)looper_prepare},
    {"ALooper_acquire",             (void*)looper_acquire},
    {"ALooper_release",             (void*)looper_release},
    {"ALooper_pollOnce",            (void*)looper_pollOnce},
    {"ALooper_pollAll",             (void*)looper_pollAll},
    {"ALooper_wake",                (void*)looper_wake},
    {"ALooper_addFd",               (void*)looper_addFd},
    {"ALooper_removeFd",            (void*)looper_removeFd},
    {"AInputQueue_getEvent",        (void*)iq_getEvent},
    {"AInputQueue_hasEvents",       (void*)iq_hasEvents},
    {"AInputQueue_finishEvent",     (void*)iq_finishEvent},
    {"AInputEvent_getType",         (void*)ev_getType},
    {"AInputEvent_getSource",       (void*)ev_getSource},
    {"AMotionEvent_getAction",      (void*)ev_getAction},
    {"AMotionEvent_getX",           (void*)ev_getX},
    {"AMotionEvent_getY",           (void*)ev_getY},
    {"AMotionEvent_getPointerCount",(void*)ev_getPointerCount},
    {"AMotionEvent_getPointerId",   (void*)ev_getPointerId},
    {"AMotionEvent_getPressure",    (void*)ev_getPressure},
    {"AMotionEvent_getSize",        (void*)ev_getSize},
    {"AKeyEvent_getKeyCode",        (void*)ev_getKeyCode},
    {"AKeyEvent_getMetaState",      (void*)ev_getMetaState},
    {"AKeyEvent_getAction",         (void*)ev_getAction},
    {"AConfiguration_new",          (void*)acfg_new},
    {"AConfiguration_delete",       (void*)acfg_delete},
    {"AConfiguration_fromAssetManager", (void*)acfg_fromAssetManager},
    {"AConfiguration_getDensity",   (void*)acfg_getDensity},
    {"AConfiguration_getOrientation",(void*)acfg_getOrientation},
    {"AConfiguration_getScreenSize",(void*)acfg_getScreenSize},
    {"AConfiguration_getSdkVersion",(void*)acfg_getSdkVersion},

    // ── unwinding / misc runtime ──────────────────────────────────────────────
    {"_Unwind_Resume",                     (void*)_Unwind_Resume},
    {"_Unwind_GetLanguageSpecificData",    (void*)_Unwind_GetLanguageSpecificData},
    {"_Unwind_GetIP",                      (void*)_Unwind_GetIP},
    {"_Unwind_SetIP",                      (void*)_Unwind_SetIP},
    {"_Unwind_GetRegionStart",             (void*)_Unwind_GetRegionStart},
    {"__gnu_unwind_frame",                 (void*)stub_gnu_unwind_frame},

    // ── EGL passthrough (switch-mesa) ─────────────────────────────────────────
    {"eglGetDisplay",       (void*)eglGetDisplay},
    {"eglInitialize",       (void*)eglInitialize},
    {"eglTerminate",        (void*)eglTerminate},
    {"eglBindAPI",          (void*)eglBindAPI},
    {"eglChooseConfig",     (void*)eglChooseConfig},
    {"eglGetConfigs",       (void*)eglGetConfigs},
    {"eglGetConfigAttrib",  (void*)eglGetConfigAttrib},
    {"eglCreateContext",    (void*)eglCreateContext},
    {"eglDestroyContext",   (void*)eglDestroyContext},
    {"eglCreateWindowSurface", (void*)eglCreateWindowSurface},
    {"eglCreatePbufferSurface",(void*)eglCreatePbufferSurface},
    {"eglDestroySurface",   (void*)eglDestroySurface},
    {"eglMakeCurrent",      (void*)eglMakeCurrent},
    {"eglSwapBuffers",      (void*)eglSwapBuffers},
    {"eglSwapInterval",     (void*)eglSwapInterval},
    {"eglGetCurrentContext",(void*)eglGetCurrentContext},
    {"eglGetCurrentSurface",(void*)eglGetCurrentSurface},
    {"eglGetCurrentDisplay",(void*)eglGetCurrentDisplay},
    {"eglQueryString",      (void*)eglQueryString},
    {"eglQuerySurface",     (void*)eglQuerySurface},
    {"eglQueryContext",     (void*)eglQueryContext},
    {"eglGetError",         (void*)eglGetError},
    {"eglGetProcAddress",   (void*)eglGetProcAddress},
    {"eglReleaseThread",    (void*)eglReleaseThread},
    {"eglWaitGL",           (void*)eglWaitGL},
    {"eglWaitClient",       (void*)eglWaitClient},
    {"eglWaitNative",       (void*)eglWaitNative},
    {"eglCopyBuffers",      (void*)eglCopyBuffers},
    {"eglBindTexImage",     (void*)eglBindTexImage},
    {"eglReleaseTexImage",  (void*)eglReleaseTexImage},

    // ── GLES 2 passthrough ────────────────────────────────────────────────────
    {"glActiveTexture",     (void*)glActiveTexture},
    {"glAttachShader",      (void*)glAttachShader},
    {"glBindAttribLocation",(void*)glBindAttribLocation},
    {"glBindBuffer",        (void*)glBindBuffer},
    {"glBindFramebuffer",   (void*)glBindFramebuffer},
    {"glBindRenderbuffer",  (void*)glBindRenderbuffer},
    {"glBindTexture",       (void*)glBindTexture},
    {"glBlendColor",        (void*)glBlendColor},
    {"glBlendEquation",     (void*)glBlendEquation},
    {"glBlendEquationSeparate",(void*)glBlendEquationSeparate},
    {"glBlendFunc",         (void*)glBlendFunc},
    {"glBlendFuncSeparate", (void*)glBlendFuncSeparate},
    {"glBufferData",        (void*)glBufferData},
    {"glBufferSubData",     (void*)glBufferSubData},
    {"glCheckFramebufferStatus",(void*)glCheckFramebufferStatus},
    {"glClear",             (void*)glClear},
    {"glClearColor",        (void*)glClearColor},
    {"glClearDepthf",       (void*)glClearDepthf},
    {"glClearStencil",      (void*)glClearStencil},
    {"glColorMask",         (void*)glColorMask},
    {"glCompileShader",     (void*)glCompileShader},
    {"glCompressedTexImage2D",  (void*)glCompressedTexImage2D},
    {"glCompressedTexSubImage2D",(void*)glCompressedTexSubImage2D},
    {"glCopyTexImage2D",    (void*)glCopyTexImage2D},
    {"glCopyTexSubImage2D", (void*)glCopyTexSubImage2D},
    {"glCreateProgram",     (void*)glCreateProgram},
    {"glCreateShader",      (void*)glCreateShader},
    {"glCullFace",          (void*)glCullFace},
    {"glDeleteBuffers",     (void*)glDeleteBuffers},
    {"glDeleteFramebuffers",(void*)glDeleteFramebuffers},
    {"glDeleteProgram",     (void*)glDeleteProgram},
    {"glDeleteRenderbuffers",(void*)glDeleteRenderbuffers},
    {"glDeleteShader",      (void*)glDeleteShader},
    {"glDeleteTextures",    (void*)glDeleteTextures},
    {"glDepthFunc",         (void*)glDepthFunc},
    {"glDepthMask",         (void*)glDepthMask},
    {"glDepthRangef",       (void*)glDepthRangef},
    {"glDetachShader",      (void*)glDetachShader},
    {"glDisable",           (void*)glDisable},
    {"glDisableVertexAttribArray",(void*)glDisableVertexAttribArray},
    {"glDrawArrays",        (void*)glDrawArrays},
    {"glDrawElements",      (void*)glDrawElements},
    {"glEnable",            (void*)glEnable},
    {"glEnableVertexAttribArray",(void*)glEnableVertexAttribArray},
    {"glFinish",            (void*)glFinish},
    {"glFlush",             (void*)glFlush},
    {"glFramebufferRenderbuffer",(void*)glFramebufferRenderbuffer},
    {"glFramebufferTexture2D",  (void*)glFramebufferTexture2D},
    {"glFrontFace",         (void*)glFrontFace},
    {"glGenBuffers",        (void*)glGenBuffers},
    {"glGenerateMipmap",    (void*)glGenerateMipmap},
    {"glGenFramebuffers",   (void*)glGenFramebuffers},
    {"glGenRenderbuffers",  (void*)glGenRenderbuffers},
    {"glGenTextures",       (void*)glGenTextures},
    {"glGetActiveAttrib",   (void*)glGetActiveAttrib},
    {"glGetActiveUniform",  (void*)glGetActiveUniform},
    {"glGetAttachedShaders",(void*)glGetAttachedShaders},
    {"glGetAttribLocation", (void*)glGetAttribLocation},
    {"glGetBooleanv",       (void*)glGetBooleanv},
    {"glGetBufferParameteriv",(void*)glGetBufferParameteriv},
    {"glGetError",          (void*)glGetError},
    {"glGetFloatv",         (void*)glGetFloatv},
    {"glGetFramebufferAttachmentParameteriv",(void*)glGetFramebufferAttachmentParameteriv},
    {"glGetIntegerv",       (void*)glGetIntegerv},
    {"glGetProgramiv",      (void*)glGetProgramiv},
    {"glGetProgramInfoLog", (void*)glGetProgramInfoLog},
    {"glGetRenderbufferParameteriv",(void*)glGetRenderbufferParameteriv},
    {"glGetShaderiv",       (void*)glGetShaderiv},
    {"glGetShaderInfoLog",  (void*)glGetShaderInfoLog},
    {"glGetShaderPrecisionFormat",(void*)glGetShaderPrecisionFormat},
    {"glGetShaderSource",   (void*)glGetShaderSource},
    {"glGetString",         (void*)glGetString},
    {"glGetTexParameterfv", (void*)glGetTexParameterfv},
    {"glGetTexParameteriv", (void*)glGetTexParameteriv},
    {"glGetUniformfv",      (void*)glGetUniformfv},
    {"glGetUniformiv",      (void*)glGetUniformiv},
    {"glGetUniformLocation",(void*)glGetUniformLocation},
    {"glGetVertexAttribfv", (void*)glGetVertexAttribfv},
    {"glGetVertexAttribiv", (void*)glGetVertexAttribiv},
    {"glGetVertexAttribPointerv",(void*)glGetVertexAttribPointerv},
    {"glHint",              (void*)glHint},
    {"glIsBuffer",          (void*)glIsBuffer},
    {"glIsEnabled",         (void*)glIsEnabled},
    {"glIsFramebuffer",     (void*)glIsFramebuffer},
    {"glIsProgram",         (void*)glIsProgram},
    {"glIsRenderbuffer",    (void*)glIsRenderbuffer},
    {"glIsShader",          (void*)glIsShader},
    {"glIsTexture",         (void*)glIsTexture},
    {"glLineWidth",         (void*)glLineWidth},
    {"glLinkProgram",       (void*)glLinkProgram},
    {"glPixelStorei",       (void*)glPixelStorei},
    {"glPolygonOffset",     (void*)glPolygonOffset},
    {"glReadPixels",        (void*)glReadPixels},
    {"glReleaseShaderCompiler",(void*)glReleaseShaderCompiler},
    {"glRenderbufferStorage",(void*)glRenderbufferStorage},
    {"glSampleCoverage",    (void*)glSampleCoverage},
    {"glScissor",           (void*)glScissor},
    {"glShaderBinary",      (void*)glShaderBinary},
    {"glShaderSource",      (void*)glShaderSource},
    {"glStencilFunc",       (void*)glStencilFunc},
    {"glStencilFuncSeparate",(void*)glStencilFuncSeparate},
    {"glStencilMask",       (void*)glStencilMask},
    {"glStencilMaskSeparate",(void*)glStencilMaskSeparate},
    {"glStencilOp",         (void*)glStencilOp},
    {"glStencilOpSeparate", (void*)glStencilOpSeparate},
    {"glTexImage2D",        (void*)glTexImage2D},
    {"glTexParameterf",     (void*)glTexParameterf},
    {"glTexParameterfv",    (void*)glTexParameterfv},
    {"glTexParameteri",     (void*)glTexParameteri},
    {"glTexParameteriv",    (void*)glTexParameteriv},
    {"glTexSubImage2D",     (void*)glTexSubImage2D},
    {"glUniform1f",         (void*)glUniform1f},
    {"glUniform1fv",        (void*)glUniform1fv},
    {"glUniform1i",         (void*)glUniform1i},
    {"glUniform1iv",        (void*)glUniform1iv},
    {"glUniform2f",         (void*)glUniform2f},
    {"glUniform2fv",        (void*)glUniform2fv},
    {"glUniform2i",         (void*)glUniform2i},
    {"glUniform2iv",        (void*)glUniform2iv},
    {"glUniform3f",         (void*)glUniform3f},
    {"glUniform3fv",        (void*)glUniform3fv},
    {"glUniform3i",         (void*)glUniform3i},
    {"glUniform3iv",        (void*)glUniform3iv},
    {"glUniform4f",         (void*)glUniform4f},
    {"glUniform4fv",        (void*)glUniform4fv},
    {"glUniform4i",         (void*)glUniform4i},
    {"glUniform4iv",        (void*)glUniform4iv},
    {"glUniformMatrix2fv",  (void*)glUniformMatrix2fv},
    {"glUniformMatrix3fv",  (void*)glUniformMatrix3fv},
    {"glUniformMatrix4fv",  (void*)glUniformMatrix4fv},
    {"glUseProgram",        (void*)glUseProgram},
    {"glValidateProgram",   (void*)glValidateProgram},
    {"glVertexAttrib1f",    (void*)glVertexAttrib1f},
    {"glVertexAttrib2f",    (void*)glVertexAttrib2f},
    {"glVertexAttrib3f",    (void*)glVertexAttrib3f},
    {"glVertexAttrib4f",    (void*)glVertexAttrib4f},
    {"glVertexAttrib1fv",   (void*)glVertexAttrib1fv},
    {"glVertexAttrib2fv",   (void*)glVertexAttrib2fv},
    {"glVertexAttrib3fv",   (void*)glVertexAttrib3fv},
    {"glVertexAttrib4fv",   (void*)glVertexAttrib4fv},
    {"glVertexAttribPointer",(void*)glVertexAttribPointer},
    {"glViewport",          (void*)glViewport},

    // ── GLES 3 passthrough ────────────────────────────────────────────────────
    {"glBeginQuery",           (void*)glBeginQuery},
    {"glBeginTransformFeedback",(void*)glBeginTransformFeedback},
    {"glBindBufferBase",       (void*)glBindBufferBase},
    {"glBindBufferRange",      (void*)glBindBufferRange},
    {"glBindSampler",          (void*)glBindSampler},
    {"glBindTransformFeedback",(void*)glBindTransformFeedback},
    {"glBindVertexArray",      (void*)glBindVertexArray},
    {"glBlitFramebuffer",      (void*)glBlitFramebuffer},
    {"glCopyBufferSubData",    (void*)glCopyBufferSubData},
    {"glDeleteQueries",        (void*)glDeleteQueries},
    {"glDeleteSamplers",       (void*)glDeleteSamplers},
    {"glDeleteSync",           (void*)glDeleteSync},
    {"glDeleteTransformFeedbacks",(void*)glDeleteTransformFeedbacks},
    {"glDeleteVertexArrays",   (void*)glDeleteVertexArrays},
    {"glDrawArraysInstanced",  (void*)glDrawArraysInstanced},
    {"glDrawBuffers",          (void*)glDrawBuffers},
    {"glDrawElementsInstanced",(void*)glDrawElementsInstanced},
    {"glEndQuery",             (void*)glEndQuery},
    {"glEndTransformFeedback", (void*)glEndTransformFeedback},
    {"glFenceSync",            (void*)glFenceSync},
    {"glFlushMappedBufferRange",(void*)glFlushMappedBufferRange},
    {"glFramebufferTextureLayer",(void*)glFramebufferTextureLayer},
    {"glGenQueries",           (void*)glGenQueries},
    {"glGenSamplers",          (void*)glGenSamplers},
    {"glGenTransformFeedbacks",(void*)glGenTransformFeedbacks},
    {"glGenVertexArrays",      (void*)glGenVertexArrays},
    {"glGetActiveUniformBlockiv",(void*)glGetActiveUniformBlockiv},
    {"glGetActiveUniformBlockName",(void*)glGetActiveUniformBlockName},
    {"glGetActiveUniformsiv",  (void*)glGetActiveUniformsiv},
    {"glGetInteger64v",        (void*)glGetInteger64v},
    {"glGetIntegeri_v",        (void*)glGetIntegeri_v},
    {"glGetFragDataLocation",  (void*)glGetFragDataLocation},
    {"glGetInternalformativ",  (void*)glGetInternalformativ},
    {"glGetStringi",           (void*)glGetStringi},
    {"glGetUniformBlockIndex", (void*)glGetUniformBlockIndex},
    {"glGetUniformuiv",        (void*)glGetUniformuiv},
    {"glInvalidateFramebuffer",(void*)glInvalidateFramebuffer},
    {"glIsQuery",              (void*)glIsQuery},
    {"glIsSampler",            (void*)glIsSampler},
    {"glIsSync",               (void*)glIsSync},
    {"glIsTransformFeedback",  (void*)glIsTransformFeedback},
    {"glIsVertexArray",        (void*)glIsVertexArray},
    {"glMapBufferRange",       (void*)glMapBufferRange},
    {"glPauseTransformFeedback",(void*)glPauseTransformFeedback},
    {"glReadBuffer",           (void*)glReadBuffer},
    {"glRenderbufferStorageMultisample",(void*)glRenderbufferStorageMultisample},
    {"glResumeTransformFeedback",(void*)glResumeTransformFeedback},
    {"glSamplerParameterf",    (void*)glSamplerParameterf},
    {"glSamplerParameterfv",   (void*)glSamplerParameterfv},
    {"glSamplerParameteri",    (void*)glSamplerParameteri},
    {"glSamplerParameteriv",   (void*)glSamplerParameteriv},
    {"glTexImage3D",           (void*)glTexImage3D},
    {"glTexStorage2D",         (void*)glTexStorage2D},
    {"glTexStorage3D",         (void*)glTexStorage3D},
    {"glTexSubImage3D",        (void*)glTexSubImage3D},
    {"glTransformFeedbackVaryings",(void*)glTransformFeedbackVaryings},
    {"glUniform1ui",           (void*)glUniform1ui},
    {"glUniform1uiv",          (void*)glUniform1uiv},
    {"glUniform2ui",           (void*)glUniform2ui},
    {"glUniform2uiv",          (void*)glUniform2uiv},
    {"glUniform3ui",           (void*)glUniform3ui},
    {"glUniform3uiv",          (void*)glUniform3uiv},
    {"glUniform4ui",           (void*)glUniform4ui},
    {"glUniform4uiv",          (void*)glUniform4uiv},
    {"glUniformBlockBinding",  (void*)glUniformBlockBinding},
    {"glUniformMatrix2x3fv",   (void*)glUniformMatrix2x3fv},
    {"glUniformMatrix2x4fv",   (void*)glUniformMatrix2x4fv},
    {"glUniformMatrix3x2fv",   (void*)glUniformMatrix3x2fv},
    {"glUniformMatrix3x4fv",   (void*)glUniformMatrix3x4fv},
    {"glUniformMatrix4x2fv",   (void*)glUniformMatrix4x2fv},
    {"glUniformMatrix4x3fv",   (void*)glUniformMatrix4x3fv},
    {"glUnmapBuffer",          (void*)glUnmapBuffer},
    {"glVertexAttribDivisor",  (void*)glVertexAttribDivisor},
    {"glVertexAttribI4i",      (void*)glVertexAttribI4i},
    {"glVertexAttribI4iv",     (void*)glVertexAttribI4iv},
    {"glVertexAttribI4ui",     (void*)glVertexAttribI4ui},
    {"glVertexAttribI4uiv",    (void*)glVertexAttribI4uiv},
    {"glVertexAttribIPointer", (void*)glVertexAttribIPointer},
    {"glWaitSync",             (void*)glWaitSync},
    {"glClientWaitSync",       (void*)glClientWaitSync},
    {"glProgramBinary",        (void*)glProgramBinary},
    {"glProgramParameteri",    (void*)glProgramParameteri},
    {"glGetProgramBinary",     (void*)glGetProgramBinary},
    {"glGetBufferPointerv",    (void*)glGetBufferPointerv},

    // ── ctype passthrough ────────────────────────────────────────────────────
    {"toupper",  (void*)toupper},
    {"tolower",  (void*)tolower},
    {"isalnum",  (void*)isalnum},
    {"isalpha",  (void*)isalpha},
    {"islower",  (void*)islower},
    {"isupper",  (void*)isupper},
    {"isdigit",  (void*)isdigit},
    {"isspace",  (void*)isspace},
    {"isprint",  (void*)isprint},
    {"iscntrl",  (void*)iscntrl},
    {"ispunct",  (void*)ispunct},
    {"isblank",  (void*)isblank},
    {"isxdigit", (void*)isxdigit},

    // ── stdio extras ─────────────────────────────────────────────────────────
    {"puts",      (void*)puts},
    {"putchar",   (void*)putchar},
    {"vsscanf",   (void*)vsscanf},
    {"vasprintf", (void*)stub_vasprintf},

    // ── string extras ────────────────────────────────────────────────────────
    {"stpcpy",    (void*)stub_stpcpy},
    {"strpbrk",   (void*)strpbrk},
    {"strcoll",   (void*)strcoll},
    {"strxfrm",   (void*)strxfrm},
    {"strerror",  (void*)strerror},
    {"strerror_r",(void*)_strerror_r},
    {"strtold",   (void*)strtold},

    // ── file I/O extras ──────────────────────────────────────────────────────
    {"rename",  (void*)rename},
    {"remove",  (void*)remove},
    {"getcwd",  (void*)stub_getcwd},
    {"fcntl",   (void*)stub_fcntl_sock},

    // ── time ─────────────────────────────────────────────────────────────────
    {"clock_gettime", (void*)clock_gettime},
    {"nanosleep",     (void*)nanosleep},
    {"gettimeofday",  (void*)gettimeofday},
    {"gmtime",        (void*)gmtime},
    {"localtime",     (void*)localtime},
    {"mktime",        (void*)mktime},
    {"strftime",      (void*)strftime},

    // ── POSIX semaphores ─────────────────────────────────────────────────────
    {"sem_init",    (void*)stub_sem_init},
    {"sem_destroy", (void*)stub_sem_destroy},
    {"sem_post",    (void*)stub_sem_post},
    {"sem_wait",    (void*)stub_sem_wait},
    {"sem_trywait", (void*)stub_sem_trywait},

    // ── pthread_mutexattr ────────────────────────────────────────────────────
    {"pthread_mutexattr_init",    (void*)pt_mattr_init},
    {"pthread_mutexattr_destroy", (void*)pt_mattr_destroy},
    {"pthread_mutexattr_settype", (void*)pt_mattr_settype},

    // ── setjmp / longjmp (ARM64: real functions, not macros) ─────────────────
    {"setjmp",  (void*)setjmp},
    {"longjmp", (void*)longjmp},

    // ── wide char (wchar.h / wctype.h) ───────────────────────────────────────
    {"wcslen",    (void*)wcslen},
    {"wcscpy",    (void*)wcscpy},
    {"wcsncpy",   (void*)wcsncpy},
    {"wcscat",    (void*)wcscat},
    {"wcsncat",   (void*)wcsncat},
    {"wcscmp",    (void*)wcscmp},
    {"wcsncmp",   (void*)wcsncmp},
    {"wcschr",    (void*)wcschr},
    {"wcsrchr",   (void*)wcsrchr},
    {"wcsstr",    (void*)wcsstr},
    {"wcstol",    (void*)wcstol},
    {"wcstoul",   (void*)wcstoul},
    {"wcstoll",   (void*)wcstoll},
    {"wcstoull",  (void*)wcstoull},
    {"wcstod",    (void*)wcstod},
    {"wcstof",    (void*)wcstof},
    {"wcstold",   (void*)wcstold},
    {"wcscoll",   (void*)wcscoll},
    {"wcsxfrm",   (void*)wcsxfrm},
    {"wcsrtombs", (void*)wcsrtombs},
    {"mbsrtowcs", (void*)mbsrtowcs},
    {"wcsnrtombs",(void*)stub_wcsnrtombs},
    {"mbsnrtowcs",(void*)stub_mbsnrtowcs},
    {"wcrtomb",   (void*)wcrtomb},
    {"mbtowc",    (void*)mbtowc},
    {"mbrtowc",   (void*)mbrtowc},
    {"mbrlen",    (void*)mbrlen},
    {"wctob",     (void*)wctob},
    {"btowc",     (void*)btowc},
    {"wmemcpy",   (void*)wmemcpy},
    {"wmemmove",  (void*)wmemmove},
    {"wmemset",   (void*)wmemset},
    {"wmemcmp",   (void*)wmemcmp},
    {"wmemchr",   (void*)wmemchr},
    {"swprintf",  (void*)swprintf},
    {"towupper",  (void*)towupper},
    {"towlower",  (void*)towlower},
    {"iswupper",  (void*)iswupper},
    {"iswlower",  (void*)iswlower},
    {"iswdigit",  (void*)iswdigit},
    {"iswalpha",  (void*)iswalpha},
    {"iswspace",  (void*)iswspace},
    {"iswpunct",  (void*)iswpunct},
    {"iswcntrl",  (void*)iswcntrl},
    {"iswprint",  (void*)iswprint},
    {"iswblank",  (void*)iswblank},
    {"iswxdigit", (void*)iswxdigit},
    {"iswgraph",  (void*)iswgraph},
    {"iswascii",  (void*)+[](wint_t c) -> int { return (unsigned)c <= 127; }},

    // ── locale ───────────────────────────────────────────────────────────────
    {"setlocale",              (void*)setlocale},
    {"localeconv",             (void*)localeconv},
    {"newlocale",              (void*)stub_newlocale},
    {"freelocale",             (void*)stub_freelocale},
    {"uselocale",              (void*)stub_uselocale},
    {"__ctype_get_mb_cur_max", (void*)stub_mb_cur_max},

    // ── strtod locale variants ───────────────────────────────────────────────
    {"strtoll_l",  (void*)stub_strtoll_l},
    {"strtoull_l", (void*)stub_strtoull_l},
    {"strtold_l",  (void*)stub_strtold_l},

    // ── networking (all stubbed — no sockets without bsd service) ────────────
    {"socket",       (void*)stub_socket},
    {"bind",         (void*)stub_bind},
    {"connect",      (void*)stub_connect},
    {"listen",       (void*)stub_listen},
    {"select",       (void*)stub_select},
    {"recv",         (void*)stub_recv},
    {"send",         (void*)stub_send},
    {"getsockname",  (void*)stub_getsockname},
    {"getsockopt",   (void*)stub_getsockopt},
    {"gethostbyname",(void*)stub_gethostbyname},
    {"__get_h_errno",(void*)stub_get_h_errno},

    // ── scheduling / system ──────────────────────────────────────────────────
    {"sched_yield",     (void*)stub_sched_yield},
    {"sysconf",         (void*)stub_sysconf},
    {"syscall",         (void*)stub_syscall},
    {"dl_iterate_phdr", (void*)stub_dl_iterate_phdr},
    {"getpid",          (void*)stub_getpid},
    {"getuid",          (void*)stub_getuid},
    {"getgid",          (void*)stub_getgid},
    {"gettid",          (void*)stub_gettid},
    {"prctl",           (void*)stub_prctl},
    {"access",          (void*)stub_access},
    {"chmod",           (void*)stub_chmod},
    {"fchmod",          (void*)stub_fchmod},
    {"lstat",           (void*)stub_lstat},
    {"mprotect",        (void*)stub_mprotect},
    {"mmap",            (void*)stub_mmap},
    {"munmap",          (void*)stub_munmap},
    {"pipe",            (void*)stub_pipe},
    {"dup",             (void*)stub_dup},
    {"dup2",            (void*)stub_dup2},
    {"ioctl",           (void*)stub_ioctl},
    {"getauxval",       (void*)stub_getauxval},
    {"sleep",           (void*)stub_sleep},
    {"usleep",          (void*)stub_usleep},
    {"clock_nanosleep", (void*)stub_clock_nanosleep},
    {"strtod_l",        (void*)stub_strtod_l},
    {"strtof_l",        (void*)stub_strtof_l},
    {"__register_atfork",(void*)stub_register_atfork},

    // ── signals (crash reporters install SIGSEGV/SIGBUS handlers) ────────────
    {"sigaction",       (void*)stub_sigaction},
    {"sigemptyset",     (void*)stub_sigemptyset},
    {"sigfillset",      (void*)stub_sigfillset},
    {"sigaddset",       (void*)stub_sigaddset},
    {"sigdelset",       (void*)stub_sigdelset},
    {"sigismember",     (void*)stub_sigismember},
    {"kill",            (void*)stub_kill},
    {"raise",           (void*)stub_raise},
    {"pthread_kill",    (void*)stub_pthread_kill},
    {"sigprocmask",     (void*)stub_sigprocmask},
    {"pthread_sigmask", (void*)stub_pthread_sigmask},

    // ── pthread extras ───────────────────────────────────────────────────────
    {"pthread_setname_np",            (void*)stub_pthread_setname_np},
    {"pthread_getname_np",            (void*)stub_pthread_getname_np},
    {"pthread_attr_setstack",         (void*)stub_pthread_attr_setstack},
    {"pthread_attr_getstack",         (void*)stub_pthread_attr_getstack},
    {"pthread_attr_setschedpolicy",   (void*)stub_pthread_attr_setschedpolicy},
    {"pthread_attr_setschedparam",    (void*)stub_pthread_attr_setschedparam},
    {"pthread_attr_getschedparam",    (void*)stub_pthread_attr_getschedparam},
    {"pthread_barrier_init",          (void*)stub_pthread_barrier_init},
    {"pthread_barrier_wait",          (void*)stub_pthread_barrier_wait},
    {"pthread_barrier_destroy",       (void*)stub_pthread_barrier_destroy},

    // ── syslog ───────────────────────────────────────────────────────────────
    {"openlog",  (void*)stub_openlog},
    {"closelog", (void*)stub_closelog},
    {"syslog",   (void*)stub_syslog},

    // ── Android specifics ────────────────────────────────────────────────────
    {"android_set_abort_message", (void*)stub_android_abort_msg},

    // ── Bionic fortified string/IO wrappers ──────────────────────────────────
    {"__strlen_chk",    (void*)chk_strlen},
    {"__memcpy_chk",    (void*)chk_memcpy},
    {"__memmove_chk",   (void*)chk_memmove},
    {"__strcat_chk",    (void*)chk_strcat},
    {"__strcpy_chk",    (void*)chk_strcpy},
    {"__strncpy_chk2",  (void*)chk_strncpy},
    {"__vsprintf_chk",  (void*)chk_vsprintf},
    {"__vsnprintf_chk", (void*)chk_vsnprintf},
    {"__read_chk",      (void*)chk_read},
    {"__open_2",        (void*)chk_open2},

    // ── sincosf ──────────────────────────────────────────────────────────────
    {"sincosf", (void*)stub_sincosf},

    // ── data symbols (address of variable, not value) ─────────────────────────
    {"__stack_chk_guard", (void*)&__stack_chk_guard},
    {"__sF",              (void*)g_fake_sF},

    // ── C++ runtime extras ───────────────────────────────────────────────────
    {"__cxa_finalize", (void*)+[](void*) {}},

    // ── Batch 3 — unresolved from 2026-06-30 run ────────────────────────────
    // crash reporter / signal handling
    {"dladdr",          (void*)stub_dladdr},
    {"sigaltstack",     (void*)stub_sigaltstack},
    {"signal",          (void*)signal},
    {"strsignal",       (void*)stub_strsignal},
    {"sys_signame",     (void*)g_sys_signames},
    // process info / environment
    {"environ",         (void*)&environ},
    {"fork",            (void*)stub_fork},
    {"execve",          (void*)stub_execve},
    {"waitpid",         (void*)stub_waitpid},
    {"_exit",           (void*)stub__exit},
    {"setuid",          (void*)stub_setuid},
    {"setgid",          (void*)stub_setgid},
    // filesystem
    {"chdir",           (void*)stub_chdir},
    {"realpath",        (void*)stub_realpath},
    {"readlink",        (void*)stub_readlink},
    {"symlink",         (void*)stub_symlink},
    {"utimes",          (void*)stub_utimes},
    {"isatty",          (void*)stub_isatty},
    {"tmpfile",         (void*)stub_tmpfile},
    // network / polling
    {"accept",          (void*)stub_accept},
    {"setsockopt",      (void*)stub_setsockopt},
    {"poll",            (void*)stub_poll},
    // stdio extras
    {"clearerr",        (void*)stub_clearerr},
    {"fileno",          (void*)stub_fileno},
    {"fdopen",          (void*)stub_fdopen},
    {"popen",           (void*)stub_popen},
    {"pclose",          (void*)stub_pclose},
    // terminal
    {"tcgetattr",       (void*)stub_tcgetattr},
    {"tcsetattr",       (void*)stub_tcsetattr},
    // time
    {"gmtime_r",        (void*)stub_gmtime_r},
    {"localtime_r",     (void*)stub_localtime_r},
    {"difftime",        (void*)stub_difftime},
    {"strptime",        (void*)stub_strptime},
    {"fesetround",      (void*)stub_fesetround},
    // math
    {"acosh",           (void*)stub_acosh},
    {"asinh",           (void*)stub_asinh},
    {"atanh",           (void*)stub_atanh},
    {"log1p",           (void*)stub_log1p},
    {"expm1",           (void*)stub_expm1},
    // memory
    {"malloc_usable_size", (void*)stub_malloc_usable_size},
    // Bionic fortified wrappers
    {"__memset_chk",    (void*)stub_memset_chk},
    {"__strchr_chk",    (void*)stub_strchr_chk},
    {"__FD_SET_chk",    (void*)stub___FD_SET_chk},
    {"__FD_ISSET_chk",  (void*)stub___FD_ISSET_chk},
    // locale-variant char classification
    {"isdigit_l",       (void*)stub_isdigit_l},
    {"islower_l",       (void*)stub_islower_l},
    {"isupper_l",       (void*)stub_isupper_l},
    {"isxdigit_l",      (void*)stub_isxdigit_l},
    {"tolower_l",       (void*)stub_tolower_l},
    {"toupper_l",       (void*)stub_toupper_l},
    {"iswalpha_l",      (void*)stub_iswalpha_l},
    {"iswblank_l",      (void*)stub_iswblank_l},
    {"iswcntrl_l",      (void*)stub_iswcntrl_l},
    {"iswdigit_l",      (void*)stub_iswdigit_l},
    {"iswlower_l",      (void*)stub_iswlower_l},
    {"iswprint_l",      (void*)stub_iswprint_l},
    {"iswpunct_l",      (void*)stub_iswpunct_l},
    {"iswspace_l",      (void*)stub_iswspace_l},
    {"iswupper_l",      (void*)stub_iswupper_l},
    {"iswxdigit_l",     (void*)stub_iswxdigit_l},
    {"towlower_l",      (void*)stub_towlower_l},
    {"towupper_l",      (void*)stub_towupper_l},
    {"strcoll_l",       (void*)stub_strcoll_l},
    {"strxfrm_l",       (void*)stub_strxfrm_l},
    {"strftime_l",      (void*)stub_strftime_l},
    {"wcscoll_l",       (void*)stub_wcscoll_l},
    {"wcsxfrm_l",       (void*)stub_wcsxfrm_l},
    // string extras that newlib provides but weren't forwarded
    {"strspn",          (void*)strspn},

    // sentinel
    {nullptr, nullptr}
};

static constexpr size_t NUM_SHIMS = sizeof(g_shims)/sizeof(g_shims[0]) - 1;

// ─── shimResolve — linear scan (fast enough for N < 400) ──────────────────────
void* shimResolve(const char* name) {
    if (!name) return nullptr;
    for (size_t i = 0; i < NUM_SHIMS; i++) {
        if (strcmp(g_shims[i].name, name) == 0)
            return g_shims[i].ptr;
    }
    return nullptr;
}
