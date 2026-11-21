#include "fake_jni.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "../audio/audiotrack.h"

#include "../loader/elf_loader.h"
#include "../loader/log.h"
#include "../shims/shim.h"

#define CLASS_BASE 0xC1000000u
#define MID_BASE 0xD1000000u
#define STR_BASE 0xE1000000u
#define MAX_CLASSES 512
#define MAX_IDS 4096
#define MAX_STRINGS 4096
#define MAX_NATIVES 1024

static uint64_t bv_jni_unimpl(int idx);

#include "jni_slots.inc"

typedef struct {
    int cls;
    char name[64];
    char sig[96];
    int kind; /* 0 method, 1 static method, 2 field, 3 static field */
    uint32_t calls; /* for the log: each method is logged for its first 5 calls, then at powers of two */
} id_rec;

static char g_classes[MAX_CLASSES][96];
static int g_nclasses;
static id_rec g_ids[MAX_IDS];
static int g_nids;
static char *g_strings[MAX_STRINGS]; /* a ring: the 4096 newest strings stay valid */
static int g_nstrings;               /* valid entries, at most MAX_STRINGS */
static uint32_t g_str_counter;       /* strings created so far */
static SceUID g_jni_mutex = -1;      /* the game's threads call JNI concurrently: guards the tables above */
#define JLOCK() do { if (g_jni_mutex >= 0) sceKernelLockMutex(g_jni_mutex, 1, NULL); } while (0)
#define JUNLOCK() do { if (g_jni_mutex >= 0) sceKernelUnlockMutex(g_jni_mutex, 1); } while (0)
static bv_native g_natives[MAX_NATIVES];
static int g_nnatives;
static uint8_t g_unimpl_seen[BV_JNI_NSLOTS];

static const void *g_env_table[BV_JNI_NSLOTS];
static const void *const *g_env = g_env_table; /* JNIEnv* == &g_env */
static const void *g_vm_table[8];
static const void *const *g_vm = g_vm_table; /* JavaVM* == &g_vm */

static uint64_t bv_jni_unimpl(int idx) {
    if (!g_unimpl_seen[idx]) {
        g_unimpl_seen[idx] = 1;
        bv_log("JNI: slot %d %s is not implemented (returns 0)", idx, bv_jni_names[idx]);
    }
    return 0;
}

static int class_index_locked(const char *name) {
    for (int i = 0; i < g_nclasses; i++)
        if (!strcmp(g_classes[i], name)) return i;
    if (g_nclasses >= MAX_CLASSES) return 0;
    strncpy(g_classes[g_nclasses], name, 95);
    return g_nclasses++;
}
static int class_index(const char *name) {
    JLOCK();
    int r = class_index_locked(name);
    JUNLOCK();
    return r;
}

static const char *class_name(uint32_t h) {
    uint32_t i = h - CLASS_BASE;
    return h >= CLASS_BASE && i < (uint32_t)g_nclasses ? g_classes[i] : "?";
}

static uint32_t new_id_locked(uint32_t cls, const char *name, const char *sig, int kind) {
    for (int i = 0; i < g_nids; i++)
        if (g_ids[i].cls == (int)(cls - CLASS_BASE) && g_ids[i].kind == kind && !strcmp(g_ids[i].name, name) && !strcmp(g_ids[i].sig, sig))
            return MID_BASE + (uint32_t)i * 4 + 4;
    if (g_nids >= MAX_IDS) return MID_BASE + 4;
    id_rec *r = &g_ids[g_nids];
    r->cls = (int)(cls - CLASS_BASE);
    r->kind = kind;
    strncpy(r->name, name, 63);
    strncpy(r->sig, sig, 95);
    return MID_BASE + (uint32_t)(g_nids++) * 4 + 4;
}
static uint32_t new_id(uint32_t cls, const char *name, const char *sig, int kind) {
    JLOCK();
    uint32_t r = new_id_locked(cls, name, sig, kind);
    JUNLOCK();
    return r;
}

static const id_rec *id_of(uint32_t h) {
    uint32_t i = (h - MID_BASE) / 4;
    return h > MID_BASE && i >= 1 && i - 1 < (uint32_t)g_nids ? &g_ids[i - 1] : NULL;
}

/* ---- implemented slots ---- */
static uint32_t jni_GetVersion(void *env) { (void)env; return 0x10006; }

static uint32_t jni_FindClass(void *env, const char *name) {
    (void)env;
    int i = class_index(name);
    bv_log("JNI FindClass(%s) -> 0x%08x", name, CLASS_BASE + (uint32_t)i);
    return CLASS_BASE + (uint32_t)i;
}
static uint32_t jni_GetObjectClass(void *env, uint32_t obj) {
    (void)env; (void)obj;
    return CLASS_BASE + (uint32_t)class_index("java/lang/Object");
}
static uint32_t jni_GetMethodID(void *env, uint32_t cls, const char *name, const char *sig) {
    (void)env;
    uint32_t id = new_id(cls, name, sig, 0);
    bv_log("JNI GetMethodID(%s, %s, %s) -> 0x%08x", class_name(cls), name, sig, id);
    return id;
}
static uint32_t jni_GetStaticMethodID(void *env, uint32_t cls, const char *name, const char *sig) {
    (void)env;
    uint32_t id = new_id(cls, name, sig, 1);
    bv_log("JNI GetStaticMethodID(%s, %s, %s) -> 0x%08x", class_name(cls), name, sig, id);
    return id;
}
static uint32_t jni_GetFieldID(void *env, uint32_t cls, const char *name, const char *sig) {
    (void)env;
    uint32_t id = new_id(cls, name, sig, 2);
    bv_log("JNI GetFieldID(%s, %s, %s) -> 0x%08x", class_name(cls), name, sig, id);
    return id;
}
static uint32_t jni_GetStaticFieldID(void *env, uint32_t cls, const char *name, const char *sig) {
    (void)env;
    uint32_t id = new_id(cls, name, sig, 3);
    bv_log("JNI GetStaticFieldID(%s, %s, %s) -> 0x%08x", class_name(cls), name, sig, id);
    return id;
}

/* Call<Type>Method[V|A], CallStatic<Type>Method[V|A]: (env, obj-or-class, methodID, ...) - logged by target */
static volatile int g_dialog_next = 1;
static volatile int g_dialog_q[8];
static volatile unsigned g_dialog_w, g_dialog_r;
int bv_jni_take_dialog(void) {
    if (g_dialog_r == g_dialog_w) return 0;
    int id = g_dialog_q[g_dialog_r % 8];
    g_dialog_r++;
    return id;
}

/* ---- Java byte arrays (FMOD's AudioTrack output hands its PCM to AudioDevice.write in a byte[]) ----
 * A handle is ARR_BASE + 4 * (index + 1). Local and global references are the same handle, so each array counts its references: New*Ref adds one,
 * Delete*Ref drops one, and the memory goes with the last one (FMOD's usual NewByteArray / NewGlobalRef / DeleteLocalRef keeps the array alive). */
#define ARR_BASE 0xB1000000u
#define MAX_ARRAYS 64
typedef struct {
    int refs; /* 0 = free slot */
    uint32_t len;
    uint8_t *data;
} jarray_rec;
static jarray_rec g_arrays[MAX_ARRAYS];
static jarray_rec *arr_of(uint32_t h) {
    uint32_t i = (h - ARR_BASE) / 4;
    if (h <= ARR_BASE || (h & 3) || i < 1 || i > MAX_ARRAYS) return NULL;
    jarray_rec *a = &g_arrays[i - 1];
    return a->refs > 0 ? a : NULL;
}
static uint32_t jni_NewByteArray(void *env, int32_t len) {
    (void)env;
    if (len < 0) return 0;
    uint8_t *d = (uint8_t *)calloc(1, (size_t)len + 1);
    if (!d) return 0;
    JLOCK();
    for (int i = 0; i < MAX_ARRAYS; i++)
        if (!g_arrays[i].refs) {
            g_arrays[i].refs = 1;
            g_arrays[i].len = (uint32_t)len;
            g_arrays[i].data = d;
            JUNLOCK();
            BV_TRACE(1, "JNI NewByteArray(%d) -> 0x%08x", (int)len, ARR_BASE + 4u * (uint32_t)(i + 1));
            return ARR_BASE + 4u * (uint32_t)(i + 1);
        }
    JUNLOCK();
    free(d);
    bv_log("JNI NewByteArray(%d): all %d array slots in use -> NULL", (int)len, MAX_ARRAYS);
    return 0;
}
static uint32_t jni_GetArrayLength(void *env, uint32_t h) {
    (void)env;
    jarray_rec *a = arr_of(h);
    return a ? a->len : 0;
}
static void *jni_GetArrayElements(void *env, uint32_t h, uint8_t *is_copy) {
    (void)env;
    if (is_copy) *is_copy = 0;
    jarray_rec *a = arr_of(h);
    return a ? a->data : NULL;
}
static void jni_ReleaseArrayElements(void *env, uint32_t h, void *p, int mode) { (void)env; (void)h; (void)p; (void)mode; }
static void jni_GetByteArrayRegion(void *env, uint32_t h, int32_t start, int32_t n, uint8_t *buf) {
    (void)env;
    jarray_rec *a = arr_of(h);
    if (a && buf && start >= 0 && n >= 0 && (uint32_t)start + (uint32_t)n <= a->len) memcpy(buf, a->data + start, (size_t)n);
    else bv_log("JNI GetByteArrayRegion(0x%08x, %d, %d): out of range or not an array", (unsigned)h, (int)start, (int)n);
}
static void jni_SetByteArrayRegion(void *env, uint32_t h, int32_t start, int32_t n, const uint8_t *buf) {
    (void)env;
    jarray_rec *a = arr_of(h);
    if (a && buf && start >= 0 && n >= 0 && (uint32_t)start + (uint32_t)n <= a->len) memcpy(a->data + start, buf, (size_t)n);
    else bv_log("JNI SetByteArrayRegion(0x%08x, %d, %d): out of range or not an array", (unsigned)h, (int)start, (int)n);
}
static uint32_t jni_NewRef(void *env, uint32_t o) {
    (void)env;
    JLOCK();
    jarray_rec *a = arr_of(o);
    if (a) a->refs++;
    JUNLOCK();
    return o;
}
static void jni_DeleteRef(void *env, uint32_t o) {
    (void)env;
    uint8_t *gone = NULL;
    JLOCK();
    jarray_rec *a = arr_of(o);
    if (a && --a->refs == 0) {
        gone = a->data;
        a->data = NULL;
        a->len = 0;
    }
    JUNLOCK();
    free(gone);
}
/* for the host test */
uint32_t bv_jni_test_new_bytes(int len) { return jni_NewByteArray(NULL, len); }
uint8_t *bv_jni_test_bytes(uint32_t h, uint32_t *len) {
    jarray_rec *a = arr_of(h);
    if (len) *len = a ? a->len : 0;
    return a ? a->data : NULL;
}

/* Number of 32-bit words each argument of a JNI signature takes (1, or 2 for J / D); returns the argument count (at most max). */
static int sig_args(const char *sig, int *wide, int max) {
    int n = 0;
    const char *p = sig ? strchr(sig, '(') : NULL;
    if (!p) return 0;
    for (p++; *p && *p != ')' && n < max;) {
        int w = (*p == 'J' || *p == 'D');
        while (*p == '[') p++;
        if (*p == 'L') { p = strchr(p, ';'); if (!p) break; }
        p++;
        wide[n++] = w;
    }
    return n;
}

static uint64_t jni_dispatch(uint32_t obj, uint32_t mid, const uint32_t *a);

/* Call<T>Method / CallStatic<T>Method: arguments as C varargs */
static uint64_t jni_callany(void *env, uint32_t obj, uint32_t mid, ...) {
    (void)env;
    uint32_t a[4] = {0, 0, 0, 0};
    int wide[4];
    const id_rec *r = id_of(mid);
    int n = r ? sig_args(r->sig, wide, 4) : 0;
    va_list ap;
    va_start(ap, mid);
    for (int i = 0; i < n; i++) a[i] = wide[i] ? (uint32_t)va_arg(ap, uint64_t) : va_arg(ap, uint32_t);
    va_end(ap);
    return jni_dispatch(obj, mid, a);
}
/* Call<T>MethodV: a va_list */
static uint64_t jni_callany_v(void *env, uint32_t obj, uint32_t mid, va_list ap) {
    (void)env;
    uint32_t a[4] = {0, 0, 0, 0};
    int wide[4];
    const id_rec *r = id_of(mid);
    int n = r ? sig_args(r->sig, wide, 4) : 0;
    va_list cp;
    va_copy(cp, ap);
    for (int i = 0; i < n; i++) a[i] = wide[i] ? (uint32_t)va_arg(cp, uint64_t) : va_arg(cp, uint32_t);
    va_end(cp);
    return jni_dispatch(obj, mid, a);
}
/* Call<T>MethodA: an array of 8-byte jvalues */
static uint64_t jni_callany_a(void *env, uint32_t obj, uint32_t mid, const uint64_t *jv) {
    (void)env;
    uint32_t a[4] = {0, 0, 0, 0};
    int wide[4];
    const id_rec *r = id_of(mid);
    int n = r && jv ? sig_args(r->sig, wide, 4) : 0;
    for (int i = 0; i < n; i++) a[i] = (uint32_t)jv[i];
    return jni_dispatch(obj, mid, a);
}

int bv_finish_exits = 1; /* config finish_exits (cycle 1, ISSUES A7) */
/* update 186: the game's keyboard requests (VirtualKeyboardHandler), taken by gfx/ime_vita.c on the game thread */
static char g_kb_text[512];
static volatile int g_kb_show, g_kb_hide, g_kb_multi, g_kb_max, g_kb_max2;
int bv_jni_kb_take(char *text, int cap, int *maxlen, int *multi) {
    if (!g_kb_show) return 0;
    g_kb_show = 0;
    if (text && cap > 0) { strncpy(text, g_kb_text, (size_t)cap - 1); text[cap - 1] = 0; }
    if (maxlen) *maxlen = g_kb_max;
    if (multi) *multi = g_kb_multi;
    return 1;
}
int bv_jni_kb_hidden(void) { int h = g_kb_hide; g_kb_hide = 0; return h; }
static const char *str_of(uint32_t h);
static uint64_t jni_dispatch(uint32_t obj, uint32_t mid, const uint32_t *a) {
    (void)obj;
    const id_rec *r = id_of(mid);
    /* update 137 (K2 news): log what the game asks the HTTP client for (run 154: two startGetRequest calls at 61 s, news screen empty) */
    if (r && r->kind == 1 && !strcmp(r->name, "startGetRequest") && !strcmp(g_classes[r->cls], "com/supercell/titan/NativeHTTPClientManager")) {
        static int said;
        if (said++ < 12) {
            const char *s0 = str_of(a[0]), *s1 = str_of(a[1]), *s2 = str_of(a[2]), *s3 = str_of(a[3]);
            bv_log("http: startGetRequest('%.160s', '%.80s', '%.80s', '%.80s')", s0 ? s0 : "?", s1 ? s1 : "?", s2 ? s2 : "?", s3 ? s3 : "?");
        }
    }
    /* FMOD's Audio Track output (audio/audiotrack.c) */
    if (r && r->kind == 0 && !strcmp(g_classes[r->cls], "org/fmod/AudioDevice")) {
        if (!strcmp(r->name, "init")) return (uint64_t)bv_audiotrack_init((int)a[0], (int)a[1], (int)a[2], (int)a[3]);
        if (!strcmp(r->name, "write")) {
            jarray_rec *arr = arr_of(a[0]);
            int n = (int)a[1];
            if (!arr) {
                static int said;
                if (!said++) bv_log("AudioDevice.write: 0x%08x is not a byte array from NewByteArray (length %d): dropped", (unsigned)a[0], n);
                return 0;
            }
            bv_audiotrack_write(arr->data, n < 0 ? 0 : ((uint32_t)n > arr->len ? (int)arr->len : n));
            return 0;
        }
        if (!strcmp(r->name, "close")) { bv_audiotrack_close(); return 0; }
    }
    /* update 186 (ISSUES W10, run 233 log): the game's text fields call the static VirtualKeyboardHandler.setMaxTextLength(II), setText([B) (UTF-8 bytes),
     * showKeyboard(ZLjava/lang/String;) and hideKeyboard(); they are recorded here and gfx/ime_vita.c opens the Vita keyboard from the frame swap */
    if (r && r->kind == 1 && !strcmp(g_classes[r->cls], "com/supercell/titan/VirtualKeyboardHandler")) {
        if (!strcmp(r->name, "setMaxTextLength")) { g_kb_max = (int)a[0] > 0 ? (int)a[0] : (int)a[1]; g_kb_max2 = (int)a[1]; }
        else if (!strcmp(r->name, "setText")) {
            jarray_rec *arr = arr_of(a[0]);
            uint32_t n = arr ? (arr->len < sizeof g_kb_text - 1 ? arr->len : (uint32_t)sizeof g_kb_text - 1) : 0;
            if (n) memcpy(g_kb_text, arr->data, n);
            g_kb_text[n] = 0;
        } else if (!strcmp(r->name, "showKeyboard")) { g_kb_multi = (int)(a[0] & 1); g_kb_hide = 0; g_kb_show = 1; }
        else if (!strcmp(r->name, "hideKeyboard")) { g_kb_show = 0; g_kb_hide = 1; }
        bv_log("keyboard: %s (args %d, %d) text '%.40s' max %d/%d", r->name, (int)a[0], (int)a[1], g_kb_text, g_kb_max, g_kb_max2);
        return 0;
    }
    if (r && r->kind == 1 && !strcmp(r->name, "ShowDialog") && !strcmp(g_classes[r->cls], "com/supercell/titan/NativeDialogManager")) {
        int id = __sync_fetch_and_add(&g_dialog_next, 1);
        g_dialog_q[g_dialog_w % 8] = id;
        g_dialog_w++;
        BV_TRACE(1, "JNI call static NativeDialogManager.ShowDialog -> dialog id %d (the loader reports it dismissed)", id);
        return (uint64_t)(uint32_t)id;
    }
    /* fmod_output experiment only (shims/shim_fmod.c): FMOD asks its Java class whether it was initialised and for the device's sample rate; with 0 / 0
     * System::init failed with 13 on the console (2026-11-11). The default (fmod_output=0, NOSOUND) keeps the old answers. */
    extern int bv_fmod_output;
    if (r && bv_fmod_output != 0 && !strcmp(g_classes[r->cls], "org/fmod/FMOD")) {
        if (!strcmp(r->name, "checkInit")) { bv_log("JNI call static org/fmod/FMOD.checkInit()Z -> 1 (fmod_output experiment)"); return 1; }
        if (!strcmp(r->name, "getOutputSampleRate")) { bv_log("JNI call static org/fmod/FMOD.getOutputSampleRate()I -> 48000 (fmod_output experiment)"); return 48000; }
    }
    /* cycle 1 (ISSUES A7): GameApp.requestFinish() is the Java side's finish() (the game asks to close, e.g. Back on a loading screen). Run 32: ignoring
     * it froze the game (no more frames) and it crashed ~20 s later. Exit to the LiveArea like Android closes the activity. */
    if (r && !strcmp(r->name, "requestFinish") && !strcmp(g_classes[r->cls], "com/supercell/titan/GameApp")) {
        extern int bv_finish_exits;
        if (bv_finish_exits) {
            bv_log("JNI GameApp.requestFinish(): the game asks to close -> exiting the app (finish_exits=1)");
            sceKernelExitProcess(0);
        }
        bv_log("JNI GameApp.requestFinish(): ignored (finish_exits=0)");
        return 0;
    }
    /* Logged for the first 5 calls of each method, then at powers of two: GameApp.beforeLogicCallback() runs every game tick, and run 12 logged it
     * 8557 times (an unbuffered memory-card write per frame on the game thread). */
    uint32_t nc = r ? __sync_add_and_fetch(&((id_rec *)r)->calls, 1) : 0;
    if (r && (nc <= 5 || !(nc & (nc - 1))))
        BV_TRACE(1, "JNI call %s%s.%s%s -> 0 (call #%u)", r->kind == 1 ? "static " : "", g_classes[r->cls], r->name, r->sig, (unsigned)nc);
    else if (r)
        ;
    else
        BV_TRACE(1, "JNI call of unknown method id 0x%08x -> 0", mid);
    return 0;
}
/* CallNonvirtual<Type>Method...: (env, obj, class, methodID, ...) */
static uint64_t jni_callnonvirtual(void *env, uint32_t obj, uint32_t cls, uint32_t mid, ...) {
    (void)env; (void)cls;
    static const uint32_t none[4];
    return jni_dispatch(obj, mid, none); /* arguments not decoded: nothing implemented is called this way */
}
/* New / NewObject: returns a fresh handle */
static uint32_t jni_NewObject(void *env, uint32_t cls, uint32_t mid, ...) {
    (void)env;
    const id_rec *r = id_of(mid);
    bv_log("JNI NewObject(%s, %s%s)", class_name(cls), r ? r->name : "?", r ? r->sig : "");
    return cls + 0x100000;
}

static uint64_t jni_getfield(void *env, uint32_t obj, uint32_t fid) {
    (void)env; (void)obj;
    const id_rec *r = id_of(fid);
    BV_TRACE(1, "JNI get field %s%s -> 0", r ? r->name : "?", r ? r->sig : "");
    return 0;
}
static void jni_setfield(void *env, uint32_t obj, uint32_t fid, ...) {
    (void)env; (void)obj;
    const id_rec *r = id_of(fid);
    BV_TRACE(1, "JNI set field %s%s", r ? r->name : "?", r ? r->sig : "");
}

static uint32_t jni_zero(void) { return 0; }
static uint32_t jni_PushLocalFrame(void *env, int cap) { (void)env; (void)cap; return 0; }
static uint32_t jni_PopLocalFrame(void *env, uint32_t r) { (void)env; return r; }
static uint32_t jni_IsSameObject(void *env, uint32_t a, uint32_t b) { (void)env; return a == b; }
static uint32_t jni_GetObjectRefType(void *env, uint32_t o) { (void)env; (void)o; return 1; }

static uint32_t jni_NewStringUTF(void *env, const char *s) {
    (void)env;
    char *dup = strdup(s ? s : "");
    if (!dup) return STR_BASE;
    static volatile uint32_t nstr;
    uint32_t ns = __sync_add_and_fetch(&nstr, 1);
    if (ns <= 200 || !(ns & (ns - 1))) BV_TRACE(1, "JNI NewStringUTF(\"%.60s\") (#%u; the first 200 are logged, then powers of two)", s ? s : "", (unsigned)ns);
    JLOCK();
    uint32_t slot = g_str_counter++ % MAX_STRINGS;
    char *old = g_strings[slot];
    g_strings[slot] = dup;
    if (g_nstrings < MAX_STRINGS) g_nstrings = (int)(g_str_counter < MAX_STRINGS ? g_str_counter : MAX_STRINGS);
    JUNLOCK();
    free(old); /* the string created MAX_STRINGS calls ago: no live reference can still use it */
    return STR_BASE + slot * 4 + 4;
}
static const char *str_of(uint32_t h) {
    uint32_t i = (h - STR_BASE) / 4;
    return h > STR_BASE && i >= 1 && i - 1 < (uint32_t)g_nstrings ? g_strings[i - 1] : "";
}
static const char *jni_GetStringUTFChars(void *env, uint32_t s, uint8_t *is_copy) {
    (void)env;
    if (is_copy) *is_copy = 0;
    return str_of(s);
}
static void jni_ReleaseStringUTFChars(void *env, uint32_t s, const char *c) { (void)env; (void)s; (void)c; }
static uint32_t jni_GetStringUTFLength(void *env, uint32_t s) { (void)env; return (uint32_t)strlen(str_of(s)); }

/* UTF-16 views (GetStringChars / GetStringLength) of the stored UTF-8 strings: the slots used to return NULL, a crash for any caller that reads them */
static uint16_t *utf16_of(const char *u, uint32_t *n_out) {
    size_t cap = strlen(u) + 1;
    uint16_t *w = (uint16_t *)malloc(cap * sizeof(uint16_t) * 2);
    if (!w) return NULL;
    uint32_t n = 0;
    for (const unsigned char *p = (const unsigned char *)u; *p;) {
        uint32_t c = *p++;
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (extra) c &= (0x3Fu >> extra);
        while (extra-- > 0 && (*p & 0xC0) == 0x80) c = (c << 6) | (*p++ & 0x3F);
        if (c >= 0x10000u) {
            c -= 0x10000u;
            w[n++] = (uint16_t)(0xD800u | (c >> 10));
            w[n++] = (uint16_t)(0xDC00u | (c & 0x3FF));
        } else {
            w[n++] = (uint16_t)c;
        }
    }
    w[n] = 0;
    *n_out = n;
    return w;
}
static uint32_t jni_GetStringLength(void *env, uint32_t s) {
    (void)env;
    uint32_t n = 0;
    uint16_t *w = utf16_of(str_of(s), &n);
    free(w);
    return n;
}
static const uint16_t *jni_GetStringChars(void *env, uint32_t s, uint8_t *is_copy) {
    (void)env;
    uint32_t n;
    if (is_copy) *is_copy = 1;
    return utf16_of(str_of(s), &n);
}
static void jni_ReleaseStringChars(void *env, uint32_t s, const uint16_t *c) {
    (void)env; (void)s;
    free((void *)c);
}

typedef struct {
    const char *name;
    const char *sig;
    void *fn;
} jni_native_method;

static int jni_RegisterNatives(void *env, uint32_t cls, const jni_native_method *m, int n) {
    (void)env;
    bv_log("JNI RegisterNatives(%s, %d methods)", class_name(cls), n);
    for (int i = 0; i < n; i++) {
        uint32_t fn = (uint32_t)(uintptr_t)m[i].fn;
        const so_module *mod = bv_module_for_pc(fn & ~1u);
        bv_log("  native %s%s -> %s+0x%x", m[i].name, m[i].sig, mod ? mod->name : "?", mod ? (fn & ~1u) - mod->load : fn);
        if (g_nnatives < MAX_NATIVES) {
            bv_native *r = &g_natives[g_nnatives++];
            strncpy(r->cls, class_name(cls), sizeof r->cls - 1);
            strncpy(r->name, m[i].name, sizeof r->name - 1);
            strncpy(r->sig, m[i].sig, sizeof r->sig - 1);
            r->fn = fn;
        }
    }
    return 0;
}
static int jni_GetJavaVM(void *env, void **vm) {
    (void)env;
    *vm = &g_vm;
    return 0;
}

/* ---- JavaVM ---- */
/* GetEnv / AttachCurrentThread are logged for the first 10 calls, then at powers of two only. With sound on, FMOD's mixer calls GetEnv for every audio
 * block: run 10 (2026-11-11) wrote 26374 GetEnv lines in 350 s (~75 unbuffered memory-card writes per second), and the game's asset reads took 2.2x longer
 * than without sound (17991 ms against 8135 ms for the same 28596 KB) while no thread was busy. */
static volatile uint32_t g_getenv_n, g_attach_n;
static int log_this(volatile uint32_t *c) {
    uint32_t n = __sync_add_and_fetch(c, 1);
    return n <= 10 || !(n & (n - 1));
}
static int vm_GetEnv(void *vm, void **penv, int version) {
    (void)vm;
    if (log_this(&g_getenv_n)) BV_TRACE(1, "JavaVM GetEnv(version 0x%x) (call #%u; logged for the first 10, then at powers of two)", version, (unsigned)g_getenv_n);
    *penv = &g_env;
    return 0;
}
static int vm_Attach(void *vm, void **penv, void *args) {
    (void)vm; (void)args;
    if (log_this(&g_attach_n)) BV_TRACE(1, "JavaVM AttachCurrentThread (call #%u; logged for the first 10, then at powers of two)", (unsigned)g_attach_n);
    *penv = &g_env;
    return 0;
}
static int vm_Detach(void *vm) { (void)vm; return 0; }

static int g_inited;
static void init_tables(void) {
    if (g_inited) return;
    g_inited = 1;
    g_jni_mutex = sceKernelCreateMutex("bv_jni", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
    memcpy(g_env_table, bv_jni_default, sizeof g_env_table);
    g_env_table[JNI_GetVersion] = jni_GetVersion;
    g_env_table[JNI_FindClass] = jni_FindClass;
    g_env_table[JNI_GetObjectClass] = jni_GetObjectClass;
    g_env_table[JNI_GetMethodID] = jni_GetMethodID;
    g_env_table[JNI_GetStaticMethodID] = jni_GetStaticMethodID;
    g_env_table[JNI_GetFieldID] = jni_GetFieldID;
    g_env_table[JNI_GetStaticFieldID] = jni_GetStaticFieldID;
    /* Call<T>Method, <T>MethodV, <T>MethodA for the ten types in the JNI order (so (slot - first) % 3 is the argument style), then the Nonvirtual family */
    static const void *const by_style[3] = {(const void *)jni_callany, (const void *)jni_callany_v, (const void *)jni_callany_a};
    for (int i = JNI_CallObjectMethod; i < JNI_GetFieldID; i++)
        g_env_table[i] = (i >= JNI_CallNonvirtualObjectMethod) ? (const void *)jni_callnonvirtual : by_style[(i - JNI_CallObjectMethod) % 3];
    for (int i = JNI_CallStaticObjectMethod; i < JNI_GetStaticFieldID; i++) g_env_table[i] = by_style[(i - JNI_CallStaticObjectMethod) % 3];
    g_env_table[JNI_NewByteArray] = jni_NewByteArray;
    g_env_table[JNI_GetArrayLength] = jni_GetArrayLength;
    g_env_table[JNI_GetByteArrayElements] = jni_GetArrayElements;
    g_env_table[JNI_ReleaseByteArrayElements] = jni_ReleaseArrayElements;
    g_env_table[JNI_GetPrimitiveArrayCritical] = jni_GetArrayElements;
    g_env_table[JNI_ReleasePrimitiveArrayCritical] = jni_ReleaseArrayElements;
    g_env_table[JNI_GetByteArrayRegion] = jni_GetByteArrayRegion;
    g_env_table[JNI_SetByteArrayRegion] = jni_SetByteArrayRegion;
    g_env_table[JNI_NewObject] = jni_NewObject;
    g_env_table[JNI_NewObjectV] = jni_NewObject;
    g_env_table[JNI_NewObjectA] = jni_NewObject;
    for (int i = JNI_GetObjectField; i <= JNI_GetDoubleField; i++) g_env_table[i] = jni_getfield;
    for (int i = JNI_SetObjectField; i <= JNI_SetDoubleField; i++) g_env_table[i] = jni_setfield;
    for (int i = JNI_GetStaticObjectField; i <= JNI_GetStaticDoubleField; i++) g_env_table[i] = jni_getfield;
    for (int i = JNI_SetStaticObjectField; i <= JNI_SetStaticDoubleField; i++) g_env_table[i] = jni_setfield;
    g_env_table[JNI_NewGlobalRef] = jni_NewRef; /* same handle; counted for byte arrays */
    g_env_table[JNI_NewLocalRef] = jni_NewRef;
    g_env_table[JNI_NewWeakGlobalRef] = jni_NewRef;
    g_env_table[JNI_DeleteGlobalRef] = jni_DeleteRef;
    g_env_table[JNI_DeleteLocalRef] = jni_DeleteRef;
    g_env_table[JNI_DeleteWeakGlobalRef] = jni_DeleteRef;
    g_env_table[JNI_ExceptionCheck] = jni_zero;
    g_env_table[JNI_ExceptionOccurred] = jni_zero;
    g_env_table[JNI_ExceptionClear] = jni_zero;
    g_env_table[JNI_ExceptionDescribe] = jni_zero;
    g_env_table[JNI_PushLocalFrame] = jni_PushLocalFrame;
    g_env_table[JNI_PopLocalFrame] = jni_PopLocalFrame;
    g_env_table[JNI_EnsureLocalCapacity] = jni_zero;
    g_env_table[JNI_IsSameObject] = jni_IsSameObject;
    g_env_table[JNI_GetObjectRefType] = jni_GetObjectRefType;
    g_env_table[JNI_NewStringUTF] = jni_NewStringUTF;
    g_env_table[JNI_GetStringUTFChars] = jni_GetStringUTFChars;
    g_env_table[JNI_ReleaseStringUTFChars] = jni_ReleaseStringUTFChars;
    g_env_table[JNI_GetStringUTFLength] = jni_GetStringUTFLength;
    g_env_table[JNI_GetStringLength] = jni_GetStringLength;
    g_env_table[JNI_GetStringChars] = jni_GetStringChars;
    g_env_table[JNI_ReleaseStringChars] = jni_ReleaseStringChars;
    g_env_table[JNI_RegisterNatives] = jni_RegisterNatives;
    g_env_table[JNI_GetJavaVM] = jni_GetJavaVM;
    g_env_table[JNI_MonitorEnter] = jni_zero;
    g_env_table[JNI_MonitorExit] = jni_zero;

    g_vm_table[3] = (const void *)jni_zero; /* DestroyJavaVM */
    g_vm_table[4] = (const void *)vm_Attach;
    g_vm_table[5] = (const void *)vm_Detach;
    g_vm_table[6] = (const void *)vm_GetEnv;
    g_vm_table[7] = (const void *)vm_Attach; /* AttachCurrentThreadAsDaemon */
    class_index("<none>");
}

uint32_t bv_jni_new_string(const char *s) { return jni_NewStringUTF(NULL, s); }

void *bv_jni_vm(void) {
    init_tables();
    return &g_vm;
}
void *bv_jni_env(void) {
    init_tables();
    return &g_env;
}
int bv_jni_native_count(void) { return g_nnatives; }
const bv_native *bv_jni_native(int i) { return i >= 0 && i < g_nnatives ? &g_natives[i] : NULL; }
const bv_native *bv_jni_find_native(const char *name) {
    for (int i = 0; i < g_nnatives; i++)
        if (!strcmp(g_natives[i].name, name)) return &g_natives[i];
    return NULL;
}
