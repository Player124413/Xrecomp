/* The in-game menu's JNI (fr.adserver.xrecomp.MenuNative, docs/android/app-shell.md « In-game menu »): hold the
 * game while the drawer is open (the lifecycle's MENU reason: the flip parks and the audio device pauses, as in the
 * background), the output volume, quit, a PlayStation game's disc change (XBA-166), and a GBA / PS1 / N64 game's
 * save states and fast-forward (XBA-182, XBA-183, recomp_states.h), and the picture changed live (scale, filter,
 * shader). Called on the UI thread. */
#include <jni.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "apu_volume.h"
#include "host_audio.h"
#include "host_window.h"
#include "ps1_runtime.h"
#include "platform/recomp_lifecycle.h"
#include "platform/recomp_quit.h"
#include "platform/recomp_states.h"
#include "video/video_player.h"

#define JNI_FN(name) Java_fr_adserver_xrecomp_MenuNative_##name

JNIEXPORT void JNICALL JNI_FN(nativeHold)(JNIEnv *env, jclass cls, jboolean on)
{
    (void)env;
    (void)cls;
    if (on)
        recomp_lifecycle_pause_for(RECOMP_HOLD_MENU);
    else
        recomp_lifecycle_resume_for(RECOMP_HOLD_MENU);
}

JNIEXPORT jboolean JNICALL JNI_FN(nativeHeld)(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    return (recomp_lifecycle_reasons() & RECOMP_HOLD_MENU) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL JNI_FN(nativeSetVolume)(JNIEnv *env, jclass cls, jint percent)
{
    (void)env;
    (void)cls;
    xbox_ApuSetOutputVolume((int)percent);   /* an Xbox game: the APU's output */
    host_audio_set_volume((int)percent);     /* a GBA / PS1 / N64 game: host_audio (XBA-181) */
}

/* Quit from the menu: the guest is let go (a flip parked by the menu must not hold anything), the saves the title
 * wrote are pushed to the disk (sync: they are plain files under <internal files>/save), then recomp_quit(0) as
 * Back twice did (exit hook, « clean exit », log drain, _exit). Does not return. */
JNIEXPORT void JNICALL JNI_FN(nativeQuit)(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    printf("xboxrecomp: quit from the in-game menu\n");
    recomp_lifecycle_resume_for(RECOMP_HOLD_ALL);
    sync();
    recomp_quit(0);
}

/* Change disc (XBA-166): the PS1 game being played gets another disc (ps1_runtime_change_disc: the lid opens, a
 * second later the disc is in and the lid closes; the runtime's thread does it between frames). path: a file path or
 * "fd:N" (a document's detached descriptor, left open). False when no PS1 game runs or path is empty. */
JNIEXPORT jboolean JNICALL JNI_FN(nativePs1ChangeDisc)(JNIEnv *env, jclass cls, jstring jpath)
{
    (void)cls;
    if (!jpath)
        return JNI_FALSE;
    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
    if (!path)
        return JNI_FALSE;
    int rc = ps1_runtime_change_disc(path);
    printf("xboxrecomp: change disc to %s from the in-game menu: %s\n", path, rc == 0 ? "queued" : "no PS1 game");
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    return rc == 0 ? JNI_TRUE : JNI_FALSE;
}

/* ---- Save states and fast-forward (XBA-182) ------------------------------------------------------------------- */
/* The running console game's slots through recomp_states.h: its runtime saves / loads between frames under its frame
 * lock (the game is held by the menu then, so it never waits for one), the slot files beside its saves. */

JNIEXPORT jboolean JNICALL JNI_FN(nativeStatesAvailable)(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    return recomp_states_available() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL JNI_FN(nativeStateSave)(JNIEnv *env, jclass cls, jint slot)
{
    (void)env;
    (void)cls;
    int rc = recomp_states_save((int)slot);
    printf("xboxrecomp: save state slot %d from the in-game menu: %s\n", (int)slot, recomp_states_error(rc));
    return rc;
}

JNIEXPORT jint JNICALL JNI_FN(nativeStateLoad)(JNIEnv *env, jclass cls, jint slot)
{
    (void)env;
    (void)cls;
    int rc = recomp_states_load((int)slot);
    printf("xboxrecomp: load state slot %d from the in-game menu: %s\n", (int)slot, recomp_states_error(rc));
    return rc;
}

/* When the slot was saved (Unix ms), 0 when it holds nothing this console's game can load: no file, damaged,
 * another format version, another console. (Another game's state is refused by the core on load.) */
JNIEXPORT jlong JNICALL JNI_FN(nativeStateTime)(JNIEnv *env, jclass cls, jint slot)
{
    (void)env;
    (void)cls;
    recomp_state_meta m;
    if (recomp_states_info((int)slot, &m, NULL) != RECOMP_STATE_OK)
        return 0;
    if (strcmp(m.console, recomp_states_console()) != 0)
        return 0;
    return m.saved_ms > 0 ? (jlong)m.saved_ms : 1;
}

/* { w, h, ARGB... } from the slot's RGB565 thumbnail, or null. */
JNIEXPORT jintArray JNICALL JNI_FN(nativeStateThumb)(JNIEnv *env, jclass cls, jint slot)
{
    (void)cls;
    static uint16_t thumb[RECOMP_STATE_THUMB_W * RECOMP_STATE_THUMB_H];
    static jint argb[2 + RECOMP_STATE_THUMB_W * RECOMP_STATE_THUMB_H];
    recomp_state_meta m;
    if (recomp_states_info((int)slot, &m, thumb) != RECOMP_STATE_OK || m.thumb_w <= 0 || m.thumb_h <= 0)
        return NULL;
    int n = m.thumb_w * m.thumb_h;
    argb[0] = m.thumb_w;
    argb[1] = m.thumb_h;
    for (int i = 0; i < n; i++) {
        unsigned c = thumb[i], r = c >> 11, g = (c >> 5) & 63, b = c & 31;
        r = r << 3 | r >> 2;
        g = g << 2 | g >> 4;
        b = b << 3 | b >> 2;
        argb[2 + i] = (jint)(0xFF000000u | r << 16 | g << 8 | b);
    }
    jintArray out = (*env)->NewIntArray(env, 2 + n);
    if (out)
        (*env)->SetIntArrayRegion(env, out, 0, 2 + n, argb);
    return out;
}

/* The last quick save / load / resume (the pad chords, the automatic slot at the start): { seq, kind, slot, rc },
 * seq 0 when nothing happened yet. The menu polls it and shows a message for each new seq. */
JNIEXPORT jintArray JNICALL JNI_FN(nativeStateEvent)(JNIEnv *env, jclass cls)
{
    (void)cls;
    recomp_state_event ev;
    recomp_states_last_event(&ev);
    jint v[4] = {(jint)ev.seq, ev.kind, ev.slot, ev.rc};
    jintArray out = (*env)->NewIntArray(env, 4);
    if (out)
        (*env)->SetIntArrayRegion(env, out, 0, 4, v);
    return out;
}

JNIEXPORT void JNICALL JNI_FN(nativeFastForward)(JNIEnv *env, jclass cls, jboolean on)
{
    (void)env;
    (void)cls;
    recomp_ff_set(on ? 1 : 0);
    printf("xboxrecomp: fast-forward %s from the in-game menu\n", on ? "on" : "off");
}

JNIEXPORT jboolean JNICALL JNI_FN(nativeFastForwardOn)(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    return recomp_ff_toggled() ? JNI_TRUE : JNI_FALSE;
}

/* The picture changed in the menu, applied at once: a GBA / PS1 / N64 game's scale, filter and shader
 * (host_window_request_picture: the next present takes them), an Xbox title's shader (the framebuffer window draws
 * the shown frame again). scale / filter: "aspect" / "integer" / "stretch", "linear" / "nearest" (null or unknown
 * keeps the current one); shader: a preset name (null or unknown: none); strength -1..100, curvature and ghosting
 * 0..100. */
JNIEXPORT void JNICALL JNI_FN(nativeSetPicture)(JNIEnv *env, jclass cls, jboolean xbox, jstring jscale,
                                                jstring jfilter, jstring jshader, jint strength, jint curvature,
                                                jint ghosting)
{
    (void)cls;
    char scale[32] = "", filter[32] = "", shader[32] = "";
    jstring js[3] = {jscale, jfilter, jshader};
    char *out[3] = {scale, filter, shader};
    for (int i = 0; i < 3; i++) {
        const char *c = js[i] ? (*env)->GetStringUTFChars(env, js[i], NULL) : NULL;
        if (c) {
            snprintf(out[i], 32, "%s", c);
            (*env)->ReleaseStringUTFChars(env, js[i], c);
        }
    }
    int preset = host_window_shader_parse(shader);
    if (preset < 0)
        preset = HOST_SHADER_NONE;
    if (xbox) {
        xbox_FramebufferWindowSetShader(preset, (int)strength, (int)curvature, (int)ghosting);
    } else {
        HostShader s = {preset, (int)strength, (int)curvature, (int)ghosting};
        host_window_request_picture(host_window_scale_parse(scale), host_window_filter_parse(filter), &s);
    }
    printf("xboxrecomp: picture from the in-game menu: %s%s%s%s shader %s\n", xbox ? "(xbox)" : "scale ",
           xbox ? "" : scale, xbox ? "" : ", filter ", xbox ? "" : filter, host_window_shader_name(preset));
}
