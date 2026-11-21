/* FMOD interposers. libfmod.so is loaded as it is; only what its Android audio backends would need is replaced.
 *
 * The Android output types (OpenSL ES, AudioTrack, AAudio) need Java and system libraries that do not exist here. Whatever the game
 * asks for, the system is switched to FMOD's own silent output (FMOD_OUTPUTTYPE_NOSOUND = 2): the mixer, channels and callbacks keep
 * working, nothing is audible. Two entry points are covered:
 *  - FMOD_System_Create (C API): after the real call, SetOutput(NOSOUND) is applied to the new system, so even a game that never
 *    calls setOutput gets the silent output;
 *  - System::setOutput (C++ API): the requested type is replaced.
 * Real audio (a custom output plugin writing to SceAudio) is a later step.
 */
#include <stdint.h>

#include "../loader/elf_loader.h"
#include "../loader/log.h"
#include "shim.h"

#define FMOD_OUTPUTTYPE_NOSOUND 2

/* The output type handed to FMOD (config fmod_output=N): 11 = FMOD_OUTPUTTYPE_AUDIOTRACK of this FMOD 2.x (default since update 30; the Java side
 * org/fmod/AudioDevice is answered by jni/fake_jni.c + audio/audiotrack.c on sceAudioOut; sound HARDWARE-VERIFIED by the user on 2026-11-11, run 9).
 * 0 = NOSOUND (silent, the behaviour up to update 23), -1 = whatever the game asks for (12 = OpenSL ES, not implemented). */
int bv_fmod_output = 11;
static int pick_output(int asked) { return bv_fmod_output == 0 ? FMOD_OUTPUTTYPE_NOSOUND : (bv_fmod_output < 0 ? asked : bv_fmod_output); }

typedef int (*set_output_fn)(void *system, int type);
typedef int (*create_fn)(void **system, uint32_t version);
typedef int (*init_fn)(void *system, int maxchannels, unsigned flags, void *extradriverdata);

static uint32_t fmod_sym(const char *name) {
    so_module *m = bv_module_by_name("libfmod.so");
    return m ? so_find_symbol(m, name) : 0;
}

static int w__ZN4FMOD6System9setOutputE15FMOD_OUTPUTTYPE(void *system, int type) {
    uint32_t real = fmod_sym("_ZN4FMOD6System9setOutputE15FMOD_OUTPUTTYPE");
    int use = pick_output(type);
    if (use == FMOD_OUTPUTTYPE_NOSOUND) BV_TRACE(1, "FMOD System::setOutput(%d) -> NOSOUND (real function %s)", type, real ? "found" : "MISSING");
    else bv_log("FMOD System::setOutput(%d) -> output type %d (fmod_output experiment; real function %s)", type, use, real ? "found" : "MISSING");
    if (!real) return 0; /* FMOD_OK */
    int r = ((set_output_fn)(uintptr_t)real)(system, use);
    if (use != FMOD_OUTPUTTYPE_NOSOUND) bv_log("FMOD System::setOutput -> %d", r);
    return r;
}

static int w_FMOD_System_Create(void **system, uint32_t version) {
    uint32_t real = fmod_sym("FMOD_System_Create");
    if (!real) {
        bv_log("FMOD_System_Create: not exported by libfmod.so");
        return 1; /* FMOD_ERR_BADCOMMAND */
    }
    int r = ((create_fn)(uintptr_t)real)(system, version);
    bv_log("FMOD_System_Create(version 0x%x) -> %d", version, r);
    if (r == 0 && system && *system) {
        uint32_t set = fmod_sym("FMOD_System_SetOutput");
        if (!set) set = fmod_sym("_ZN4FMOD6System9setOutputE15FMOD_OUTPUTTYPE");
        if (bv_fmod_output != 0) {
            bv_log("FMOD: fmod_output=%d: the new system is NOT switched to NOSOUND", bv_fmod_output);
        } else if (set) {
            int r2 = ((set_output_fn)(uintptr_t)set)(*system, FMOD_OUTPUTTYPE_NOSOUND);
            bv_log("FMOD: output switched to NOSOUND -> %d", r2);
        } else {
            bv_log("FMOD: no SetOutput function found in libfmod.so: output left on auto-detect");
        }
    }
    return r;
}

/* System::init is passed through; its result is logged, because every sound the game loads fails with FMOD error 67 ("System::init ... was not called") when it did not succeed. */
static int fmod_init_logged(const char *what, uint32_t real, void *system, int maxch, unsigned flags, void *extra) {
    if (!real) {
        bv_log("FMOD %s: the real function is MISSING", what);
        return 0;
    }
    int r = ((init_fn)(uintptr_t)real)(system, maxch, flags, extra);
    bv_log("FMOD %s(maxchannels %d, flags 0x%x) -> %d%s", what, maxch, flags, r, r ? "  <== FMOD ERROR: sounds will fail to load" : " (ok)");
    return r;
}

static int w__ZN4FMOD6System4initEijPv(void *system, int maxch, unsigned flags, void *extra) {
    return fmod_init_logged("System::init", fmod_sym("_ZN4FMOD6System4initEijPv"), system, maxch, flags, extra);
}

static int w_FMOD_System_Init(void *system, int maxch, unsigned flags, void *extra) {
    return fmod_init_logged("FMOD_System_Init", fmod_sym("FMOD_System_Init"), system, maxch, flags, extra);
}

const bv_shim bv_shims_fmod[] = {
    SHIM(_ZN4FMOD6System4initEijPv),
    SHIM(FMOD_System_Init),
    SHIM(_ZN4FMOD6System9setOutputE15FMOD_OUTPUTTYPE),
    SHIM(FMOD_System_Create),
    SHIM_END};
