/* n64_vk_cache.h -- where the N64 Vulkan RDP (n64_rdp_vk.cpp) keeps its compiled shaders between starts.
 *
 * Without it, every start compiled the same pipelines again (Ocarina of Time at 4x on the AYN Thor's Turnip: ~1.35 s
 * of the 1.85 s between the driver's open and the start line, and the same mid-game compiles of 8-62 ms every session).
 *
 * RECOMP_VK_CACHE_DIR (the app sets <cache>/vk_cache/<driver id or "system">/, ShellActivity; unset or empty: no file
 * is read or written, as before) holds:
 *   pipelines-<pipelineCacheUUID, 32 hex digits>.bin  the VkPipelineCache's data (Granite's blob: the UUID, a hash,
 *                                                     then the driver's data) behind our own header (below). A driver
 *                                                     update changes the UUID: a new file, the old one stays unused.
 *   mesa/                                             MESA_SHADER_CACHE_DIR, Mesa's own disk cache (Turnip), set
 *                                                     before the driver opens unless the environment already has it.
 * The directory is created when missing. The file is written to a temporary name next to it, then renamed: a start
 * killed while writing leaves the previous file whole. A file that is truncated, of another format, of another device
 * or driver (UUID), or whose checksum fails is ignored (logged): the cache starts empty and the file is replaced at the
 * next save.
 *
 * File layout, little-endian: "XRVKPC1\n" (8 bytes), the pipelineCacheUUID (16), the payload's size (u64), the
 * payload's FNV-1a 64 (u64), the payload. Plain C, no Vulkan: the tests run it anywhere (test_vk_driver.c). */
#ifndef N64_VK_CACHE_H
#define N64_VK_CACHE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define N64_VK_CACHE_UUID_SIZE 16
#define N64_VK_CACHE_HEADER 40                 /* magic, UUID, size, checksum */
#define N64_VK_CACHE_MAX ((size_t)256 << 20)   /* a larger payload is not read */

/* $RECOMP_VK_CACHE_DIR with a trailing '/', created when missing (parents too). 0: unset, empty, too long, or it
 * cannot be created (logged). */
int n64_vk_cache_dir(char *out, size_t n);

/* MESA_SHADER_CACHE_DIR=<dir>mesa (created) unless MESA_SHADER_CACHE_DIR is already set; nothing without a cache
 * directory. Before the driver is opened (Mesa reads it when the device is created). */
void n64_vk_cache_mesa_env(void);

/* <dir>pipelines-<uuid hex>.bin. 0: no cache directory. */
int n64_vk_cache_file(char *out, size_t n, const uint8_t uuid[N64_VK_CACHE_UUID_SIZE]);

/* The payload of the file at path when it is whole and of this UUID (malloc'd, *size its bytes), else NULL with *why
 * one of "none" (no file), "truncated", "not a cache file", "another device or driver", "checksum", "too large",
 * "out of memory", "unreadable". */
void *n64_vk_cache_load(const char *path, const uint8_t uuid[N64_VK_CACHE_UUID_SIZE], size_t *size, const char **why);

/* Writes the payload to path through a temporary file and a rename. 0 on success, -1 (logged; the old file stays). */
int n64_vk_cache_save(const char *path, const uint8_t uuid[N64_VK_CACHE_UUID_SIZE], const void *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* N64_VK_CACHE_H */
