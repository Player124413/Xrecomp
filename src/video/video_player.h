/**
 * Shared FMV player: Media Foundation decode onto a D3D8 texture.
 *
 * For a title whose video is a container Windows can already decode, the
 * decoder does not have to be emulated for the video to be watchable. The
 * recompiled title still decides when: it opens the file as its own logic
 * dictates, and the runtime plays the file it opened.
 */

#ifndef XBOXRECOMP_VIDEO_PLAYER_H
#define XBOXRECOMP_VIDEO_PLAYER_H

#include <stdint.h>

/* Initialize/shutdown Media Foundation (call once at app start/end) */
int  video_init(void);
void video_shutdown(void);

/* Open a video file for playback. Returns 0 on success. */
int  video_open(const char *path);

/* Advance playback by dt seconds, decode next frame if needed.
 * Returns 1 if a new frame is ready, 0 if unchanged, -1 if finished. */
int  video_update(float dt);

/* Render the current video frame as a fullscreen quad.
 * Call between BeginScene/EndScene. */
void video_render(void);

/* Check if the video has finished playing. */
int  video_is_finished(void);

/* Close the current video and release resources. */
void video_close(void);

/* === Boot sequence state machine === */

/* Boot phases */

/* Get current boot phase */

/* Advance boot state machine. Call once per frame.
 * skip=1 if user pressed a button to skip current video.
 * Returns the new phase. */

/* Render current boot phase (video frame or press-start screen). */


/* Play one video file, start to finish, in its own window on its own thread.
 * Returns 0 if the pump started. Blocking work happens on that thread, so the
 * guest carries on running while the video plays. */
int  xbox_VideoPlayFile(const char *host_path);
int  xbox_VideoIsPlaying(void);

/* Write the current decoded frame to a 24-bit BMP -- evidence that real
 * pixels reached the renderer, which a log line cannot give. */
int  video_dump_frame_bmp(const char *path);

/* Show the guest framebuffer in its own window (RECOMP_FB_WINDOW). Whatever
 * the title renders into guest RAM appears there; nothing else scans it out. */
void xbox_FramebufferWindowStart(void);
void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch);
/* Rows per displayed row: 2 for a 2x2 anti-aliased surface, else 1. */
void xbox_FramebufferWindowSetAA(uint32_t sy);
/* SET_SURFACE_FORMAT colour format of the surface the following presents
 * hold, as its bytes are in guest memory: 1 or 2 (X1R5G5B5) show 16-bit
 * pixels as 555, anything else as R5G6B5. */
void xbox_FramebufferWindowSetFormat(uint32_t color_format);
/* A GPU back end's own frame, not through guest memory: w x h pixels, rows
 * pitch bytes apart, top row first, in an XBOX_FB_HOST_* format (alpha
 * ignored). Any size: the window scales it into its logical 640x480
 * (linearly unless it maps 1:1). Called by the executor at FLIP_STALL
 * (Nv2aPresentSurface.host_px); the pixels are copied before it returns.
 * Returns 1 if a window took the frame, 0 if there is none (headless) or
 * the format is unknown. The Win32 GDI window takes none. */
#define XBOX_FB_HOST_BGRX8888 1u    /* bytes B, G, R, X (desktop GL GL_BGRA) */
#define XBOX_FB_HOST_RGBX8888 2u    /* bytes R, G, B, X (GLES GL_RGBA) */
int  xbox_FramebufferWindowPresentHost(const void *px, uint32_t w, uint32_t h,
                                       uint32_t pitch, uint32_t format);
/* The frame last shown, as a 24-bit BMP at its own size (640x480, or the
 * back end's size for a host frame). */
int  xbox_FramebufferDumpBmp(const char *path);
/* Title bar: "<XBE title> | FPS: n | draws: n". The name is the certificate's
 * UTF-16 title (40 chars max); the stats come from each flip. */
void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars);
void xbox_FramebufferWindowFrameStats(uint32_t draws);
/* POSIX window: the rate the title bar shows (frames per second x 100, 0
 * before the first second) and the last frame's draw count. */
void xbox_FramebufferWindowGetStats(uint32_t *fps_centi, uint32_t *draws);

/* Aspect modes (aspect_policy.h). RECOMP_ASPECT: 0 auto (default), 1 4:3,
 * 2 16:9; every configuration. */
int  xbox_AspectSetting(void);
/* The title draws its 3D frames anamorphically for 16:9 (it widened its
 * camera): the window shows frames with many triangles at 16:9 and the rest
 * (menus, movies) at 4:3. Any thread, any time. No-op on Win32. */
void xbox_FramebufferWindowSetWide(int on);
/* Triangles the frame about to be presented drew (the executor, once per
 * flip, before the present). No-op on Win32. */
void xbox_FramebufferWindowFrameTris(uint32_t tris);

#if !defined(_WIN32)
/* POSIX hosts: the framebuffer window needs the process main thread (AppKit
 * on macOS), so a title that wants it calls this from main() with its boot
 * sequence. With RECOMP_FB_WINDOW set it opens the window, runs guest_main on
 * a pthread and pumps events until guest_main returns (its value is returned)
 * or the window is closed (the process exits). Otherwise, or headless, it
 * just calls guest_main. */
int  xbox_HostWindowMain(int (*guest_main)(void));
/* Picture shaders (XBA-203, present_shaders.h): the preset the window draws its frames through (HOST_SHADER_*:
 * RECOMP_XBOX_SHADER, else RECOMP_SHADER; none before the window opens, without a GL renderer or after the shader
 * failed to build). Any thread. */
int  xbox_FramebufferWindowShaderActive(void);
/* A live change of the picture shader (the in-game menu), any thread: preset HOST_SHADER_* (lcd draws none here),
 * strength -1..100, curvature and ghosting 0..100. The window takes it at its next turn and draws the shown frame
 * again, also while the game is held. */
void xbox_FramebufferWindowSetShader(int preset, int strength, int curvature, int ghosting);
/* The window as drawn (shader included, a binary PPM of the whole drawable) is written to path after the next
 * frame it draws; one capture per call, any thread. Screenshots (xbox_FramebufferDumpBmp) stay the raw frame. */
void xbox_FramebufferWindowCapture(const char *path);
#endif

#endif /* XBOXRECOMP_VIDEO_PLAYER_H */
