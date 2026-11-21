/* Logging JavaVM / JNIEnv for the Android libraries.
 *
 * libg has no plaintext JNI names (they are obfuscated or decoded at run time), so this is how the Java-side
 * surface is discovered: every FindClass / GetMethodID / GetStaticMethodID / GetFieldID / RegisterNatives is logged
 * with its name and signature, and every Call*Method is logged by the method it targets.
 */
#ifndef BV_FAKE_JNI_H
#define BV_FAKE_JNI_H

#include <stdint.h>

typedef struct bv_native {
    char cls[96];
    char name[64];
    char sig[96];
    uint32_t fn; /* run-time address inside the native library */
} bv_native;

void *bv_jni_vm(void);  /* JavaVM* */
void *bv_jni_env(void); /* JNIEnv* */

/* A jstring handle for a C string (kept for the life of the process). */
uint32_t bv_jni_new_string(const char *s);

/* Dialogs: NativeDialogManager.ShowDialog returns a dialog id, and the loader reports every one of them as dismissed (GameApp.dialogDismissed), because there is no
 * dialog to show. Returns the next id waiting to be reported, or 0. */
int bv_jni_take_dialog(void);

int bv_jni_native_count(void);
const bv_native *bv_jni_native(int i);
const bv_native *bv_jni_find_native(const char *name);

#endif
