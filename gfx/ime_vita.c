/* update 186 (ISSUES W10): the game's text fields on the Vita keyboard.
 * The game asks Java for its keyboard (static VirtualKeyboardHandler.setMaxTextLength / setText / showKeyboard / hideKeyboard, recorded by
 * jni/fake_jni.c). Here, on the game thread from the frame swap, a pending request opens the system IME dialog with the field's text; while it is
 * open vitaGL lets the dialog draw (vglSwapBuffers(GL_TRUE)). OK hands the text back through the game's own natives, the way Android's
 * EditText would: inputTextChanged(String) then inputOkPressed(); Cancel calls inputKeyboardDismissed(). */
#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/libime.h>
#include <stdint.h>
#include <string.h>

#include "../jni/fake_jni.h"
#include "../loader/log.h"

int bv_jni_kb_take(char *text, int cap, int *maxlen, int *multi);
int bv_jni_kb_hidden(void);

volatile int bv_ime_open;   /* 1 while the dialog is up (touch_vita.c keeps touches away from the game) */
static SceWChar16 g_init[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1], g_buf[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1], g_title[8];

static int utf8_to_16(const char *s, SceWChar16 *o, int cap) {
    int n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && n < cap - 2) {
        uint32_t c = *p++;
        if (c >= 0xf0 && p[0] && p[1] && p[2]) { c = ((c & 7) << 18) | ((p[0] & 63u) << 12) | ((p[1] & 63u) << 6) | (p[2] & 63u); p += 3; }
        else if (c >= 0xe0 && p[0] && p[1]) { c = ((c & 15) << 12) | ((p[0] & 63u) << 6) | (p[1] & 63u); p += 2; }
        else if (c >= 0xc0 && p[0]) { c = ((c & 31) << 6) | (p[0] & 63u); p += 1; }
        if (c >= 0x10000) { c -= 0x10000; o[n++] = (SceWChar16)(0xd800 + (c >> 10)); o[n++] = (SceWChar16)(0xdc00 + (c & 0x3ff)); }
        else o[n++] = (SceWChar16)c;
    }
    o[n] = 0;
    return n;
}
static int utf16_to_8(const SceWChar16 *s, char *o, int cap) {
    int n = 0;
    for (; *s && n < cap - 5; s++) {
        uint32_t c = *s;
        if (c >= 0xd800 && c < 0xdc00 && s[1] >= 0xdc00 && s[1] < 0xe000) { c = 0x10000 + ((c - 0xd800) << 10) + (s[1] - 0xdc00); s++; }
        if (c < 0x80) o[n++] = (char)c;
        else if (c < 0x800) { o[n++] = (char)(0xc0 | (c >> 6)); o[n++] = (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { o[n++] = (char)(0xe0 | (c >> 12)); o[n++] = (char)(0x80 | ((c >> 6) & 63)); o[n++] = (char)(0x80 | (c & 63)); }
        else { o[n++] = (char)(0xf0 | (c >> 18)); o[n++] = (char)(0x80 | ((c >> 12) & 63)); o[n++] = (char)(0x80 | ((c >> 6) & 63)); o[n++] = (char)(0x80 | (c & 63)); }
    }
    o[n] = 0;
    return n;
}

static void call_native(const char *name, uint32_t str, int with_str) {
    const bv_native *nat = bv_jni_find_native(name);
    if (!nat || !nat->fn) { bv_log("keyboard: native %s not registered", name); return; }
    void *env = bv_jni_env();
    if (with_str) ((void (*)(void *, uint32_t, uint32_t))(uintptr_t)nat->fn)(env, 0, str);
    else ((void (*)(void *, uint32_t))(uintptr_t)nat->fn)(env, 0);
}

/* called before every swap on the game thread; returns 1 while a system dialog is up (vglSwapBuffers(GL_TRUE)) */
int bv_ime_frame(void) {
    if (!bv_ime_open) {
        char text[512];
        int maxlen = 0, multi = 0;
        bv_jni_kb_hidden();   /* a hide without an open dialog: nothing to do */
        if (!bv_jni_kb_take(text, sizeof text, &maxlen, &multi)) return 0;
        if (maxlen <= 0 || maxlen > SCE_IME_DIALOG_MAX_TEXT_LENGTH) maxlen = 128;
        utf8_to_16(text, g_init, maxlen + 1);
        memset(g_buf, 0, sizeof g_buf);
        g_title[0] = 0;
        SceImeDialogParam p;
        sceImeDialogParamInit(&p);
        p.supportedLanguages = 0;   /* the system language */
        p.languagesForced = SCE_FALSE;
        p.type = SCE_IME_TYPE_DEFAULT;
        /* update 186b (user): MULTILINE turns Enter into a new-line key and the dialog has no OK at all: always one line, Enter = OK
         * (labelled Send for the game's chat fields, which ask with showKeyboard(true, ..)) */
        p.option = 0;
        p.enterLabel = multi ? SCE_IME_ENTER_LABEL_SEND : SCE_IME_ENTER_LABEL_DEFAULT;
        p.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
        p.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;   /* WITH_CLEAR + MULTILINE: 0x80102004 (run 234) */
        p.title = g_title;
        p.maxTextLength = (SceUInt32)maxlen;
        p.initialText = g_init;
        p.inputTextBuffer = g_buf;
        int r = sceImeDialogInit(&p);
        if (r < 0) {
            bv_log("keyboard: sceImeDialogInit failed 0x%08x: the field is closed", (unsigned)r);
            call_native("inputKeyboardDismissed", 0, 0);
            return 0;
        }
        bv_ime_open = 1;
        bv_log("keyboard: Vita keyboard open (max %d characters%s, text '%.40s')", maxlen, multi ? ", Send key" : "", text);
        return 1;
    }
    if (bv_jni_kb_hidden()) { sceImeDialogAbort(); bv_log("keyboard: the game closed the field: dialog aborted"); }
    SceCommonDialogStatus st = sceImeDialogGetStatus();
    if (st == SCE_COMMON_DIALOG_STATUS_RUNNING) return 1;
    if (st != SCE_COMMON_DIALOG_STATUS_FINISHED) return 1;
    SceImeDialogResult res;
    memset(&res, 0, sizeof res);
    sceImeDialogGetResult(&res);
    sceImeDialogTerm();
    bv_ime_open = 0;
    if (res.button == SCE_IME_DIALOG_BUTTON_ENTER) {
        char out[SCE_IME_DIALOG_MAX_TEXT_LENGTH * 4 + 1];
        static char keep[2048];
        utf16_to_8(g_buf, out, (int)sizeof out);
        strncpy(keep, out, sizeof keep - 1);
        keep[sizeof keep - 1] = 0;
        uint32_t js = bv_jni_new_string(keep);
        bv_log("keyboard: OK -> inputTextChanged('%.60s') + inputOkPressed()", keep);
        call_native("inputTextChanged", js, 1);
        call_native("inputOkPressed", 0, 0);
    } else {
        bv_log("keyboard: closed without OK -> inputKeyboardDismissed()");
        call_native("inputKeyboardDismissed", 0, 0);
    }
    return 1;   /* the dialog's last frame */
}
