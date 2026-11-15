/* n64_rdp_backend.h -- the RDP back ends behind n64_devices.h (XBA-173), internal to consoles/n64/runtime.
 *
 * n64_rdp_select.c implements the n64_rdp_* functions of n64_devices.h and n64_rdp.h: n64_rdp_create picks a back end
 * (n64_rdp.h: n64_rdp_set_backend, $RECOMP_N64_RDP) and every call goes through its table:
 *   software  n64_rdp.c (or the placeholder n64_rdp_stub.c), compiled with n64_rdp_sw_names.h forced in, so its
 *             public functions are n64_rdp_sw_* and its handle struct n64_rdp_sw: both RDPs link side by side;
 *   vulkan    n64_rdp_vk.cpp: paraLLEl-RDP (third_party/parallel-rdp) on a Vulkan compute device, built when
 *             XBR_N64_RDP_VULKAN is ON (N64_RDP_HAVE_VULKAN defined for n64_rdp_select.c).
 * A test may also reach both directly (consoles/n64/tests/test_rdp_vk.c compares their RDRAM). */
#ifndef N64_RDP_BACKEND_H
#define N64_RDP_BACKEND_H

#include "n64_devices.h"
#include "n64_rdp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Save states (XBA-183): what every back end has, the DPC front end (n64_rdp_select.c writes it, then the back end's own
 * bytes). pend: the command being collected, 64-bit words high word first (npend of them). */
typedef struct n64_rdp_common_state {
    uint32_t start, end, current, status;
    uint32_t clock, bufbusy, pipebusy, tmemc;
    uint32_t start_pending, npend;
    uint64_t pend[24];
    int32_t fe_cycle, fe_ci_size, fe_ci_width, fe_zon, fe_sc_y0, fe_sc_y1;
    uint32_t fe_ci_addr, fe_zi_addr, fe_ti_addr;
    int32_t fe_ti_size, fe_ti_width;
} n64_rdp_common_state;

typedef struct n64_rdp_ops {
    const char *name; /* "software", "vulkan" */
    void *(*create)(n64_rcp_bus bus); /* NULL: this back end cannot run here */
    void (*destroy)(void *d);
    void (*reset)(void *d);
    void (*attach_dmem)(void *d, const uint8_t *dmem);
    uint32_t (*read_reg)(void *d, uint32_t reg);
    void (*write_reg)(void *d, uint32_t reg, uint32_t v);
    void (*run)(void *d);
    int (*set_threads)(void *d, int n);
    void (*flush)(void *d);
    /* Non-zero once this back end can no longer draw (the Vulkan RDP: a shader the driver failed to compile during
     * the game); n64_rdp_select.c then goes on with the software RDP. NULL: never. */
    int (*failed)(void *d);
    /* Internal resolution (XBA-185, n64_rdp.h): the factor drawn at (NULL: 1), the scanout's output pixels per VI
     * pixel (NULL or 0: no scanout) and the scanout itself (NULL: none). */
    int (*upscale)(void *d);
    int (*scanout_scale)(void *d);
    int (*scanout)(void *d, const uint32_t *vi, uint32_t field, uint32_t *xrgb, int max_w, int max_h, int *w, int *h);
    /* Save states (XBA-183), between frames: the back end's own bytes after the common part (state_extra_size; NULL:
     * none), state_save draws out what is queued and fills both, state_load puts them back: extra NULL for a state of
     * another back end (the common part only; the back end's drawing state starts from its reset values). 0, or -1
     * (the extra bytes are not this build's). */
    size_t (*state_extra_size)(void *d);
    void (*state_save)(void *d, n64_rdp_common_state *c, uint8_t *extra);
    int (*state_load)(void *d, const n64_rdp_common_state *c, const uint8_t *extra, size_t n);
    /* Host performance counters (n64_rdp.h n64_rdp_get_stats) since this back end's create. NULL: none. */
    void (*stats)(void *d, n64_rdp_stats *out);
    /* n64_rdp.h n64_rdp_unreported: 1 when rows drawn but not yet reported through bus.wrote may overlap RDRAM
     * [lo, hi). NULL: the back end is flushed instead (its pixels in RDRAM and reported). */
    int (*unreported)(void *d, uint32_t lo, uint32_t hi);
    /* n64_rdp.h n64_rdp_dp_complete (NULL: the back end never defers; n64_rdp_select.c raises the interrupt). */
    void (*dp_complete)(void *d);
    /* n64_rdp.h n64_rdp_scanout_prefetch / n64_rdp_scanout_forget (NULL: none). */
    void (*scanout_prefetch)(void *d, const uint32_t *vi, uint32_t field, uint32_t lo, uint32_t hi);
    void (*scanout_forget)(void *d);
    /* n64_rdp.h n64_rdp_scanout_rgba (NULL: none; the back end's scanout op then serves n64_rdp_scanout alone). */
    int (*scanout_rgba)(void *d, const uint32_t *vi, uint32_t field, const uint8_t **rgba, int *w, int *h, int *pitch);
} n64_rdp_ops;

/* The factor the next back end is asked to draw at (n64_rdp_select.c: n64_rdp_set_upscale, else the environment). */
int n64_rdp_upscale_wanted(void);

extern const n64_rdp_ops n64_rdp_sw_ops; /* n64_rdp_select.c, over n64_rdp_sw_* */
#ifdef N64_RDP_HAVE_VULKAN
extern const n64_rdp_ops n64_rdp_vk_ops; /* n64_rdp_vk.cpp */
void n64_rdp_vk_save_caches(const char *why); /* n64_rdp.h n64_rdp_save_caches, for the live Vulkan RDP */
#endif

#ifdef __cplusplus
}
#endif

#endif
