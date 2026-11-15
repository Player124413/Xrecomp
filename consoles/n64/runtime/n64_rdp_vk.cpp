/* n64_rdp_vk.cpp -- the Vulkan RDP (XBA-173): the DPC front end of n64_devices.h over paraLLEl-RDP (Themaister, MIT,
 * third_party/parallel-rdp), an LLE RDP on Vulkan compute that aims at angrylion's output bit for bit. A back end of
 * n64_rdp_select.c (n64_rdp_backend.h): n64_rdp_vk_ops.create returns NULL when there is no Vulkan loader, no device,
 * or a device without what paraLLEl-RDP needs (Vulkan 1.1, 8 / 16-bit storage), and the software RDP takes over.
 *
 * Front end: the same DPC registers as n64_rdp.c (START / END / CURRENT / STATUS with start pending, XBUS from DMEM,
 * freeze, CLOCK / BUFBUSY / PIPEBUSY / TMEM counters approximated from the commands the same way): whole commands are
 * collected from RDRAM or DMEM and queued to paraLLEl-RDP's command thread as 32-bit words, high word first.
 *
 * RDRAM: the core's own array (big-endian bytes) is handed to paraLLEl-RDP as host RDRAM, without
 * VK_EXT_external_memory_host (its "incoherent" path): before a render pass it copies the pages the commands read into
 * its GPU copy, after the pass it writes back, byte-masked, the bytes the GPU wrote (third_party/parallel-rdp patch:
 * every word is byte-swapped on the way, the GPU copy is in host words). Coherence with the CPU:
 *   - SYNC_FULL: the commands are flushed and the CPU waits for the GPU and for the write-back (wait_for_timeline),
 *     then the DP interrupt is raised: whatever the game reads after the interrupt (frame buffer, RDRAM the recompiled
 *     code may run from) is in the array. When the core defers it (bus.defer_dp, n64_core.c « Deferred DP
 *     interrupt »), only the timeline is signalled; the wait, the rows' report, the busy bits and the interrupt come at
 *     the core's deadline (dp_finish through n64_rdp_dp_complete), or when a second SYNC_FULL comes first;
 *   - n64_rdp_flush (the VI is about to read the frame buffer and rows drawn there are not in RDRAM yet, a save
 *     state): commands queued since the last SYNC_FULL, and a deferred SYNC_FULL's, are drawn and waited for (its
 *     interrupt still at its deadline);
 *   - early scanout (n64_rdp_scanout_prefetch): at a VI_ORIGIN change with nothing queued, the upscaled scanout is
 *     submitted at once into one of two buffers; vk_scanout waits for its fence instead of draining the command ring
 *     behind the next picture's work, unless the registers differ or rows were drawn where it reads since;
 *   - CPU writes (textures, command lists) are seen by the GPU when the commands that read them are flushed, which is
 *     at the latest at SYNC_FULL: as on the hardware, the game must not change what the RDP has not read yet.
 * Write tracking: the colour and Z image rows the drawing commands cover (scissored) are reported through bus.wrote
 * once they are in RDRAM (after a wait), never before, as n64_rdp.c does.
 *
 * Drivers that cannot compile a shader (Adreno 740 on the AYN Thor: one compute pipeline failed at every dispatch,
 * black screen at 15 fps): at create, a few commands are drawn into a corner of RDRAM (saved and restored) and must
 * compile and come out right (check()). If not, paraLLEl-RDP is tried again without subgroup operations, then with its
 * ubershader (not on Qualcomm's driver: ubershader_draws_wrong), then create returns NULL and the software RDP takes
 * over. A failed pipeline is never compiled again
 * (our Granite patch); one that still fails during the game is logged once and reported through vk_failed, and
 * n64_rdp_select.c switches to the software RDP. RECOMP_N64_RDP_CHECK=0 skips the check.
 *
 * Custom driver (Mesa Turnip on the Thor's Adreno, n64_vk_driver.h): when the app selected one (RECOMP_VK_DRIVER_DIR),
 * its vkGetInstanceProcAddr (libadrenotools, libxba_vkdriver.so) is the first loader, and the same tries run on it.
 * If it cannot be opened, makes no device, or fails every try, the system's libvulkan is loaded again and the tries
 * run there: custom driver -> system driver -> software RDP. The start line names the driver (driverName, driverInfo,
 * custom <id> or system).
 *
 * Internal resolution (XBA-185, n64_rdp.h): with a factor of 2, 4 or 8 (n64_rdp_upscale_wanted) paraLLEl-RDP draws
 * each frame-buffer pass twice, at 1x into RDRAM as above (what the game reads back) and upscaled into GPU buffers of
 * RDRAM's size times the factor squared, which its own VI scans out (vk_scanout_rgba: into one of two kept host
 * buffers, read where the GPU copied them by n64_display / n64_display_rgba). The
 * factor is lowered first when those buffers exceed the device's storage buffer range or half its largest
 * device-local heap (fit_upscale, logged). The check at create then draws into a 256-pixel-wide frame buffer (the
 * width paraLLEl-RDP upscales from) and scans it out, so the upscaled and VI pipelines are compiled and checked too;
 * a try that fails upscaled is made again at 1x.
 *
 * Pipeline cache (n64_vk_cache.h): with RECOMP_VK_CACHE_DIR, the device's VkPipelineCache starts from
 * pipelines-<pipelineCacheUUID>.bin there (before the first command processor, so the check's pipelines come from it)
 * and the cache is written back when it grew: after the check passed, every 10 s from a thread of its own (the
 * compiles during the game), when the app pauses (n64_rdp_save_caches: sessions on Android end killed after a pause)
 * and at destroy. MESA_SHADER_CACHE_DIR points Mesa's own disk cache (Turnip) at <dir>mesa before the driver opens.
 *
 * Threads (n64_rdp.h n64_rdp_set_thread_hook): paraLLEl-RDP's command ring worker, fence executor and pipeline
 * compile worker call the same hook as the software RDP's band threads when they start and end (RDP::set_thread_hook,
 * our patch in third_party/parallel-rdp): on Android they join the performance session, nice -8 and the fast cores
 * after a resume. */
#include "n64_rdp_backend.h"
#include "n64_rdp.h"
#include "n64_vk_cache.h"
#include "n64_vk_driver.h"

#include "command_buffer.hpp"
#include "context.hpp"
#include "device.hpp"
#include "rdp_device.hpp"
#include "thread_hook.hpp"
#include "thread_id.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace {

enum : uint32_t {
    ST_XBUS = 0x001,
    ST_FREEZE = 0x002,
    ST_FLUSH = 0x004,
    ST_GCLK = 0x008,
    ST_PIPE_BUSY = 0x020,
    ST_CMD_BUSY = 0x040,
    ST_CBUF_READY = 0x080,
    ST_DMA_BUSY = 0x100,
    ST_START_PENDING = 0x400,
};
enum { CYC_1 = 0, CYC_2 = 1, CYC_COPY = 2, CYC_FILL = 3 };

struct VkRdp {
    n64_rcp_bus bus{};
    const uint8_t *dmem = nullptr;
    uint32_t start = 0, end = 0, current = 0, status = 0;
    uint32_t clock = 0, bufbusy = 0, pipebusy = 0, tmemc = 0;
    int start_pending = 0;
    uint32_t pend[44]{}; /* one command, 32-bit words (high word first), up to 22 64-bit words */
    int npend = 0;       /* 64-bit words collected */
    bool unsynced = false; /* commands queued since the last wait */
    /* Deferred DP interrupt (n64_devices.h bus.defer_dp): dp_owed from a deferred SYNC_FULL to n64_rdp_dp_complete
     * (busy bits kept, no interrupt yet); dp_gpu while its work (timeline dp_timeline) is not waited for; dp_wr its
     * rows, reported once waited; dp_after: commands queued since (they keep the busy bits). */
    bool dp_owed = false, dp_gpu = false, dp_after = false;
    uint64_t dp_timeline = 0;
    uint32_t dp_wr_lo[2]{}, dp_wr_hi[2]{};
    n64_rdp_stats stats{}; /* host counters (n64_rdp_get_stats), not in save states */
    uint64_t stalls0 = 0, stall_ns0 = 0; /* get_stalled_compiles at create (vk_stats reports the difference) */

    /* Front-end view of the state, for the costs and the written ranges (as in n64_rdp.c). */
    int fe_cycle = 0, fe_ci_size = 2, fe_ci_width = 320, fe_zon = 0, fe_sc_y0 = 0, fe_sc_y1 = 0xFFF;
    uint32_t fe_ci_addr = 0, fe_zi_addr = 0;
    uint32_t wr_lo[2]{}, wr_hi[2]{};

    /* Save states (XBA-183): the last of each state command paraLLEl-RDP was given (single-word commands by id, the
     * tiles' Set Tile and their size, which the loads set too), replayed into it when a state loads. TMEM is not
     * kept: games load their textures again before they draw with them. */
    uint64_t shadow[0x40]{};
    uint64_t shadow_tile[8]{}, shadow_size[8]{};
    uint64_t shadow_valid = 0; /* bit id: shadow[id]; bit 0..7 of shadow_tiles / shadow_sizes: per tile */
    uint32_t shadow_tiles = 0, shadow_sizes = 0;

    std::unique_ptr<Vulkan::Context> ctx;
    std::unique_ptr<Vulkan::Device> dev;
    std::unique_ptr<RDP::CommandProcessor> cp;
    RDP::CommandProcessorFlags cp_flags = 0;
    bool failed = false; /* a pipeline failed during the game: its draws are dropped (vk_failed) */

    /* Internal resolution (XBA-185): the factor drawn at, the scanout's halvings, the scanout's pixels. */
    int upscale = 1;
    unsigned down_steps = 0;
    std::vector<RDP::RGBA> scan;
    /* Early scanout (n64_rdp_scanout_prefetch): two buffers taken in turn, the one prefetched (pf_valid), the
     * registers, field and RDRAM range it is of, pf_dirty once the RDP drew into that range. */
    RDP::VIScanoutBuffer pf[2];
    int pf_cur = 0;
    bool pf_valid = false, pf_dirty = false;
    uint32_t pf_vi[14]{}, pf_field = 0, pf_lo = 0, pf_hi = 0;

    /* Pipeline cache file (n64_vk_cache.h): its path ("": none), the size of the data it holds (Granite's blob), the
     * thread that saves it when it grew (pc_cv, pc_quit under g_pc_mu). */
    std::string pc_path;
    size_t pc_size = 0;
    bool pc_no_data_logged = false;
    std::thread pc_thread;
    std::condition_variable pc_cv;
    bool pc_quit = false;
};

/* Granite records command buffers per registered thread index: whichever thread drives the RDP is index 0, like
 * paraLLEl-RDP's own command and timeline threads (one thread at a time drives a machine). */
void on_thread()
{
    static thread_local bool registered;
    if (!registered) {
        Util::register_thread_index(0);
        registered = true;
    }
}

inline int imin(int a, int b) { return a < b ? a : b; }
inline int imax(int a, int b) { return a > b ? a : b; }
inline int32_t sext(uint32_t v, int bits) { return (int32_t)(v << (32 - bits)) >> (32 - bits); }

int cmd_len(int id)
{
    if (id >= 0x08 && id <= 0x0F)
        return 4 + ((id & 4) ? 8 : 0) + ((id & 2) ? 8 : 0) + ((id & 1) ? 2 : 0);
    if (id == 0x24 || id == 0x25)
        return 2;
    return 1;
}

void wrote_add(VkRdp *d, int y0, int y1)
{
    y0 = imax(y0, d->fe_sc_y0 >> 2);
    y1 = imin(y1, d->fe_sc_y1 >> 2);
    if (y0 < 0)
        y0 = 0;
    if (y1 < y0)
        return;
    const uint32_t stride[2] = { ((uint32_t)d->fe_ci_width << d->fe_ci_size) >> 1, (uint32_t)d->fe_ci_width * 2u };
    const uint32_t base[2] = { d->fe_ci_addr, d->fe_zi_addr };
    const int n = d->fe_zon && d->fe_cycle != CYC_FILL && d->fe_cycle != CYC_COPY ? 2 : 1;
    for (int i = 0; i < n; i++) {
        uint32_t lo = base[i] + (uint32_t)y0 * stride[i], hi = base[i] + (uint32_t)(y1 + 1) * stride[i];
        if (d->pf_valid && lo < d->pf_hi && d->pf_lo < hi)
            d->pf_dirty = true; /* the prefetched scanout no longer shows what is there */
        if (d->wr_hi[i] <= d->wr_lo[i]) {
            d->wr_lo[i] = lo;
            d->wr_hi[i] = hi;
        } else {
            d->wr_lo[i] = lo < d->wr_lo[i] ? lo : d->wr_lo[i];
            d->wr_hi[i] = hi > d->wr_hi[i] ? hi : d->wr_hi[i];
        }
    }
}

void wrote_report(VkRdp *d)
{
    for (int i = 0; i < 2; i++) {
        if (d->wr_hi[i] > d->wr_lo[i] && d->bus.wrote)
            d->bus.wrote(d->bus.ctx, d->wr_lo[i], d->wr_hi[i] - d->wr_lo[i]);
        d->wr_lo[i] = d->wr_hi[i] = 0;
    }
}

/* A deferred SYNC_FULL's rows, once its work is waited for. */
void dp_wrote_report(VkRdp *d)
{
    for (int i = 0; i < 2; i++) {
        if (d->dp_wr_hi[i] > d->dp_wr_lo[i] && d->bus.wrote)
            d->bus.wrote(d->bus.ctx, d->dp_wr_lo[i], d->dp_wr_hi[i] - d->dp_wr_lo[i]);
        d->dp_wr_lo[i] = d->dp_wr_hi[i] = 0;
    }
}

/* Everything queued is drawn and written back into the core's RDRAM. */
uint64_t host_ns()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void check_failure(VkRdp *d)
{
    if (!d->failed && d->cp->has_pipeline_failure()) {
        d->failed = true;
        fprintf(stderr, "n64: Vulkan RDP: a shader failed to compile on %s during the game (the Granite lines "
                        "above name it); switching to the software RDP\n",
                d->dev->get_gpu_properties().deviceName);
    }
}

/* Everything queued is drawn and written back into the core's RDRAM, a deferred SYNC_FULL's work included (whose DP
 * interrupt still comes at its deadline: dp_finish). */
void sync(VkRdp *d)
{
    if (d->unsynced || d->dp_gpu) {
        on_thread();
        const uint64_t t0 = host_ns();
        d->cp->wait_for_timeline(d->unsynced ? d->cp->signal_timeline() : d->dp_timeline);
        d->stats.waits++;
        d->stats.wait_ns += host_ns() - t0;
        d->unsynced = false;
        d->dp_gpu = false;
        check_failure(d);
    }
    dp_wrote_report(d);
    wrote_report(d);
}

/* The deferred SYNC_FULL completes (n64_rdp_dp_complete, or a second SYNC_FULL before the deadline): its work waited
 * for, its rows reported, the busy bits cleared (unless commands came since), the DP interrupt. */
void dp_finish(VkRdp *d)
{
    if (!d->dp_owed)
        return;
    d->dp_owed = false;
    if (d->dp_gpu) {
        on_thread();
        const uint64_t t0 = host_ns();
        d->cp->wait_for_timeline(d->dp_timeline);
        const uint64_t dt = host_ns() - t0;
        d->stats.dp_wait_ns += dt;
        d->stats.dp_waits += dt >= 100000; /* 0.1 ms: the GPU was still at it */
        d->dp_gpu = false;
        check_failure(d);
    }
    dp_wrote_report(d);
    if (!d->unsynced)
        d->cp->begin_frame_context(); /* recycles the per-frame Vulkan objects (nothing is in flight now) */
    if (!d->dp_after)
        d->status &= ~(ST_PIPE_BUSY | ST_GCLK);
    d->dp_after = false;
    d->stats.dp_completed++;
    if (d->bus.raise)
        d->bus.raise(d->bus.ctx, N64_MI_DP);
}

/* ---- Internal resolution (XBA-185) ---------------------------------------------------------------------------------- */
int env_int(const char *name, int def)
{
    const char *e = getenv(name);
    return e && *e ? atoi(e) : def;
}

RDP::CommandProcessorFlags upscale_flags(int f)
{
    RDP::CommandProcessorFlags fl = f >= 8   ? RDP::COMMAND_PROCESSOR_FLAG_UPSCALING_8X_BIT
                                    : f >= 4 ? RDP::COMMAND_PROCESSOR_FLAG_UPSCALING_4X_BIT
                                    : f >= 2 ? RDP::COMMAND_PROCESSOR_FLAG_UPSCALING_2X_BIT
                                             : 0;
    if (fl && env_int("RECOMP_N64_UPSCALE_SSREADBACK", 0) != 0)
        fl |= RDP::COMMAND_PROCESSOR_FLAG_SUPER_SAMPLED_READ_BACK_BIT;
    return fl;
}

/* The largest factor up to f whose buffers the device can hold: paraLLEl-RDP binds the upscaled RDRAM (RDRAM's size
 * x f^2) as one storage buffer, so it must fit maxStorageBufferRange (128 MB on many mobile drivers: 4x at most with
 * 8 MB of RDRAM, 2x with the super-sampled readback's write mask), and the upscaled RDRAM, hidden RDRAM and the VI's images must fit in half the largest device-local
 * heap (the rest is the game's and the system's; on a phone that heap is the shared RAM). */
int fit_upscale(VkRdp *d, int f)
{
    if (f <= 1)
        return 1;
    const VkPhysicalDeviceLimits &lim = d->dev->get_gpu_properties().limits;
    const VkPhysicalDeviceMemoryProperties &mem = d->dev->get_memory_properties();
    uint64_t heap = 0;
    for (uint32_t i = 0; i < mem.memoryHeapCount; i++)
        if (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            heap = std::max<uint64_t>(heap, mem.memoryHeaps[i].size);
    const uint64_t rdram = d->bus.rdram_size;
    const int asked = f;
    for (; f > 1; f /= 2) {
        const uint64_t ff = (uint64_t)f * (uint64_t)f;
        const uint64_t ss = (upscale_flags(f) & RDP::COMMAND_PROCESSOR_FLAG_SUPER_SAMPLED_READ_BACK_BIT)
                                ? 2ull * RDP::Limits::MaxWidth * RDP::Limits::MaxHeight / 8 /* its write mask */
                                : 0;
        const uint64_t range = rdram * ff + ss;
        const uint64_t images = 4ull * (640ull * (uint64_t)f) * (576ull * (uint64_t)f) * 4ull; /* VI stages, RGBA8 */
        const uint64_t need = range + rdram / 2 * ff + rdram + images;
        if (range > lim.maxStorageBufferRange) {
            fprintf(stderr, "n64: Vulkan RDP: upscale %dx needs a %llu MB storage buffer, %s allows %llu MB\n", f,
                    (unsigned long long)(range >> 20), d->dev->get_gpu_properties().deviceName,
                    (unsigned long long)(lim.maxStorageBufferRange >> 20));
            continue;
        }
        if (heap && need > heap / 2) {
            fprintf(stderr, "n64: Vulkan RDP: upscale %dx needs about %llu MB of video memory, %s has %llu MB\n", f,
                    (unsigned long long)(need >> 20), d->dev->get_gpu_properties().deviceName,
                    (unsigned long long)(heap >> 20));
            continue;
        }
        break;
    }
    if (f != asked)
        fprintf(stderr, "n64: Vulkan RDP: upscale lowered from %dx to %dx\n", asked, f);
    return f;
}

/* Halvings of the scanout: $RECOMP_N64_UPSCALE_DOWNSCALE (0..3, at most down to 1x), else as many as keep the picture
 * at most 2560 pixels wide (640 VI pixels x the factor): 8x is read back at 4x, super-sampled. */
unsigned scanout_steps(int f)
{
    unsigned steps = 0, max_steps = 0;
    while ((f >> (max_steps + 1)) >= 1 && max_steps < 3)
        max_steps++;
    const int e = env_int("RECOMP_N64_UPSCALE_DOWNSCALE", -1);
    if (e >= 0)
        steps = (unsigned)e;
    else
        while (640 * (f >> steps) > 2560)
            steps++;
    return std::min(steps, max_steps);
}

/* The game's command processor: quirks for the upscaled draws (n64_rdp.h). */
void set_quirks(VkRdp *d)
{
    if (d->upscale <= 1)
        return;
    RDP::Quirks q;
    q.set_native_resolution_tex_rect(env_int("RECOMP_N64_UPSCALE_NATIVE_TEXRECT", 1) != 0);
    q.set_native_texture_lod(env_int("RECOMP_N64_UPSCALE_NATIVE_LOD", 0) != 0);
    d->cp->set_quirks(q);
}

/* The VI's picture from paraLLEl-RDP's VI (d->scan); sw / sh 0 when it shows nothing. */
void scanout_rgb(VkRdp *d, const uint32_t *vi, uint32_t field, unsigned *sw, unsigned *sh)
{
    for (unsigned r = 0; r < unsigned(RDP::VIRegister::Count); r++)
        d->cp->set_vi_register(RDP::VIRegister(r), r == unsigned(RDP::VIRegister::VCurrentLine) ? field : vi[r]);
    RDP::ScanoutOptions so;
    so.downscale_steps = d->down_steps;
    const uint64_t t0 = host_ns();
    d->cp->scanout_sync(d->scan, *sw, *sh, so);
    const uint64_t dt = host_ns() - t0;
    d->stats.waits++;
    d->stats.wait_ns += dt;
    d->stats.scanouts++;
    d->stats.scanout_ns += dt;
}

/* ---- The check at create -------------------------------------------------------------------------------------------- */
/* A fill of the colour and Z images, a texture loaded and drawn as a rectangle with a Z test: tile binning, the
 * rasterizer (or the ubershader), depth / blend, TMEM loads and the RDRAM write-back all run. The commands are
 * encoded as in consoles/n64/tests/rdp_cmd.h. Upscaled (XBA-185), the images are 256 pixels wide (32 x 32 drawn),
 * the narrowest paraLLEl-RDP renders upscaled, and the VI then scans the colour image out (check_scanout). */
constexpr uint32_t CHECK_W = 32, CHECK_SPAN = 0x2000; /* 32 x 32 RGBA16 colour, Z, an 8 x 8 texture */
constexpr uint32_t CHECK_W_UP = 256, CHECK_SPAN_UP = 0x9000; /* the same, 256-pixel lines */
constexpr uint32_t CHECK_FILL = 0xF801F801u;           /* red, coverage bit */

void put(std::vector<uint32_t> &l, uint64_t w)
{
    l.push_back((uint32_t)(w >> 32));
    l.push_back((uint32_t)w);
}

uint64_t op(int id) { return (uint64_t)id << 56; }

/* The upscaled VI scanout of the check's colour image (16-bit, 256 wide, 32 lines shown from fb line 0, X scale 0.5,
 * replicated, no filters): the expected size, and the fill colour at frame-buffer pixel (6, 4). */
bool check_scanout(VkRdp *d, uint32_t base)
{
    const uint32_t vi[14] = { 0x302 /* 16-bit, AA mode 3 */, base, CHECK_W_UP, 0x200, 0, 0x03E52239, 0x20D, 0xC15,
                              0x0C150C15, 108u << 16 | 748u, 37u << 16 | 101u, 0x000E0204, 0x200, 0x400 };
    unsigned sw = 0, sh = 0;
    scanout_rgb(d, vi, 0, &sw, &sh);
    const unsigned s = unsigned(d->upscale) >> d->down_steps;
    if (d->cp->has_pipeline_failure())
        return false;
    if (sw != 640 * s || sh != 240 * s) {
        fprintf(stderr, "n64: Vulkan RDP: check scanout is %u x %u, expected %u x %u\n", sw, sh, 640 * s, 240 * s);
        return false;
    }
    /* canvas x = VI pixel (H_START at the canvas' left), 2 per fb pixel; row 1 is active line 0 ((37 - 34) / 2) */
    const RDP::RGBA &px = d->scan[(size_t)(5 * s + s / 2) * sw + (13 * s + s / 2)];
    if (px.r < 0xC0 || px.g > 0x40 || px.b > 0x40) {
        fprintf(stderr, "n64: Vulkan RDP: check scanout drew %02x%02x%02x (expected ff0000)\n", px.r, px.g, px.b);
        return false;
    }
    return true;
}

bool check(VkRdp *d)
{
    if (const char *e = getenv("RECOMP_N64_RDP_CHECK"))
        if (atoi(e) == 0)
            return !d->cp->has_pipeline_failure();
    const bool up = d->upscale > 1;
    const uint32_t W = up ? CHECK_W_UP : CHECK_W, span = up ? CHECK_SPAN_UP : CHECK_SPAN;
    const uint32_t base = d->bus.rdram_size - span; /* colour, then Z, then the texture */
    const uint32_t z = base + W * CHECK_W * 2, tex = z + W * CHECK_W * 2;
    std::vector<uint8_t> saved(d->bus.rdram + base, d->bus.rdram + base + span);
    for (uint32_t i = 0; i < 128; i++) /* the texture: 8 x 8 RGBA16, opaque */
        d->bus.rdram[tex + i] = (uint8_t)(i & 1 ? 0xFF : 0x7C);

    const uint64_t c10 = (uint64_t)(CHECK_W * 4), last = (uint64_t)((CHECK_W - 1) * 4);
    std::vector<uint32_t> l;
    put(l, op(0x2D) | c10 << 12 | c10);                                                     /* scissor 32 x 32 */
    put(l, op(0x2F) | 3ull << 52);                                                          /* fill mode */
    put(l, op(0x3F) | 2ull << 51 | (uint64_t)(W - 1) << 32 | z);                            /* colour image = Z */
    put(l, op(0x37) | 0xFFFCFFFCu);                                                         /* far */
    put(l, op(0x36) | last << 44 | last << 32);                                             /* fill rectangle */
    put(l, op(0x27));                                                                       /* sync pipe */
    put(l, op(0x3F) | 2ull << 51 | (uint64_t)(W - 1) << 32 | base);                         /* colour image */
    put(l, op(0x3E) | z);                                                                   /* Z image */
    put(l, op(0x37) | CHECK_FILL);
    put(l, op(0x36) | last << 44 | last << 32);
    put(l, op(0x27));
    put(l, op(0x3D) | 2ull << 51 | 7ull << 32 | tex);                                       /* texture image */
    put(l, op(0x35) | 2ull << 51 | 2ull << 41);                                             /* tile 0, line 2 */
    put(l, op(0x34) | (7ull * 4) << 12 | 7ull * 4);                                         /* load tile 8 x 8 */
    put(l, op(0x26));                                                                       /* sync load */
    put(l, op(0x3C) | 15ull << 52 | 31ull << 47 | 7ull << 44 | 7ull << 41 | 15ull << 37 | 31ull << 32 | 15ull << 28 |
               15ull << 24 | 7ull << 21 | 7ull << 18 | 1ull << 15 | 7ull << 12 | 6ull << 9 | 1ull << 6 | 7ull << 3 |
               6ull);                                                                       /* combine: texel 0 */
    put(l, op(0x2E) | 0x100ull << 16);                                                      /* prim depth */
    put(l, op(0x2F) | 1ull << 43 | 1ull << 42 | 3ull << 38 | 3ull << 36 | 1ull << 5 | 1ull << 4 | 1ull << 2);
    put(l, op(0x24) | (24ull * 4) << 44 | (24ull * 4) << 32 | (8ull * 4) << 12 | 8ull * 4); /* texture rectangle */
    put(l, 1ull << 26 | 1ull << 10);                                                        /* s, t 0; 1 texel / px */
    put(l, op(0x29));                                                                       /* sync full */
    for (size_t i = 0; i < l.size();) {
        const int id = (int)(l[i] >> 24) & 0x3F;
        const unsigned words = (unsigned)cmd_len(id) * 2;
        d->cp->enqueue_command(words, &l[i]);
        i += words;
    }
    d->cp->wait_for_timeline(d->cp->signal_timeline());
    d->cp->begin_frame_context();

    const uint8_t *fb = d->bus.rdram + base;
    const uint32_t far_px = ((CHECK_W - 1) * W + CHECK_W - 1) * 2; /* the last pixel: filled, outside the rect. */
    const uint32_t tex_px = (12 * W + 12) * 2; /* inside the rectangle: a texel, not the fill colour */
    const bool fill_ok = fb[0] == 0xF8 && fb[1] == 0x01 && fb[far_px] == 0xF8 && fb[far_px + 1] == 0x01;
    const bool tex_ok = fb[tex_px] == 0x7C && (fb[tex_px + 1] & 0xFE) == 0xFE; /* low bit: coverage */
    const bool pipelines_ok = !d->cp->has_pipeline_failure();
    if (pipelines_ok && !(fill_ok && tex_ok))
        fprintf(stderr, "n64: Vulkan RDP: check drew %02x%02x (fill, expected f801) and %02x%02x (texture, expected "
                        "7cff)\n", fb[0], fb[1], fb[tex_px], fb[tex_px + 1]);
    const bool scan_ok = !up || !(pipelines_ok && fill_ok && tex_ok) || check_scanout(d, base);
    memcpy(d->bus.rdram + base, saved.data(), span);
    return pipelines_ok && fill_ok && tex_ok && scan_ok;
}

uint32_t cmd_cost(VkRdp *d, const uint32_t *w, int id)
{
    const uint64_t w0 = (uint64_t)w[0] << 32 | w[1];
    const uint32_t per = d->fe_cycle == CYC_2 ? 2 : 1;
    if (id >= 0x08 && id <= 0x0F) {
        int yh = sext((uint32_t)w0 & 0x3FFF, 14), yl = sext((uint32_t)(w0 >> 32) & 0x3FFF, 14);
        int64_t xh = (int32_t)w[4], xm = (int32_t)w[6], xl = (int32_t)w[2];
        int64_t wd = (llabs(xm - xh) + llabs(xl - xh)) >> 17;
        int64_t rows = imax(yl - yh, 0) >> 2;
        int64_t c = rows * (wd + 1) * per + 24;
        return c > 0xFFFFFF ? 0xFFFFFF : (uint32_t)c;
    }
    if (id == 0x24 || id == 0x25 || id == 0x36) {
        int xl = (int)(w0 >> 44) & 0xFFF, yl = (int)(w0 >> 32) & 0xFFF, xh = (int)(w0 >> 12) & 0xFFF, yh = (int)w0 & 0xFFF;
        uint32_t px = (uint32_t)(imax((xl >> 2) - (xh >> 2), 0) + 1) * (uint32_t)(imax((yl >> 2) - (yh >> 2), 0) + 1);
        if (d->fe_cycle == CYC_FILL)
            return px / (d->fe_ci_size == 3 ? 2 : 4) + 8;
        if (d->fe_cycle == CYC_COPY)
            return px / 4 + 8;
        return px * per + 8;
    }
    return 1;
}

/* Save states: a state command into the shadow (see VkRdp). */
void shadow_record(VkRdp *d, uint64_t w0)
{
    const int id = (int)(w0 >> 56) & 0x3F;
    const int tile = (int)(w0 >> 24) & 7;
    if ((id >= 0x2A && id <= 0x2F) || (id >= 0x37 && id <= 0x3F) || id == 0x3C) {
        d->shadow[id] = w0;
        d->shadow_valid |= 1ull << id;
    } else if (id == 0x35) {
        d->shadow_tile[tile] = w0;
        d->shadow_tiles |= 1u << tile;
    } else if (id == 0x30 || id == 0x32 || id == 0x33 || id == 0x34) { /* the loads set the tile's size fields too */
        d->shadow_size[tile] = (w0 & ~(0x3Full << 56)) | (0x32ull << 56);
        d->shadow_sizes |= 1u << tile;
    }
}

void dispatch(VkRdp *d, const uint32_t *w, int len)
{
    const uint64_t w0 = (uint64_t)w[0] << 32 | w[1];
    const int id = (int)(w0 >> 56) & 0x3F;
    uint32_t cost = cmd_cost(d, w, id);
    d->clock = (d->clock + cost) & 0xFFFFFF;
    d->bufbusy = (d->bufbusy + cost) & 0xFFFFFF;
    const bool draw = (id >= 0x08 && id <= 0x0F) || id == 0x24 || id == 0x25 || id == 0x36;
    if (draw)
        d->pipebusy = (d->pipebusy + cost) & 0xFFFFFF;
    if (id != 0x29)
        d->status |= ST_PIPE_BUSY | ST_GCLK;
    d->stats.commands++;
    d->stats.full_syncs += id == 0x29;
    if (id == 0x30 || id == 0x33 || id == 0x34) {
        uint32_t sh = (uint32_t)(w0 >> 12) & 0xFFF, sl = (uint32_t)(w0 >> 44) & 0xFFF;
        d->tmemc = (d->tmemc + ((sh - sl) >> 2) + 4) & 0xFFFFFF;
    }
    switch (id) {
    case 0x2F: d->fe_cycle = (int)(w0 >> 52) & 3; d->fe_zon = (w0 >> 4 & 3) != 0; break;
    case 0x2D: d->fe_sc_y0 = (int)(w0 >> 32) & 0xFFF; d->fe_sc_y1 = (int)w0 & 0xFFF; break;
    case 0x3E: d->fe_zi_addr = (uint32_t)w0 & 0xFFFFFF; break;
    case 0x3F:
        d->fe_ci_addr = (uint32_t)w0 & 0xFFFFFF;
        d->fe_ci_size = (int)(w0 >> 51) & 3;
        d->fe_ci_width = (int)((w0 >> 32) & 0x3FF) + 1;
        break;
    default: break;
    }
    if (draw) {
        if (id <= 0x0F)
            wrote_add(d, sext((uint32_t)w0 & 0x3FFF, 14) >> 2, sext((uint32_t)(w0 >> 32) & 0x3FFF, 14) >> 2);
        else
            wrote_add(d, (int)(w0 & 0xFFF) >> 2, ((int)(w0 >> 32) & 0xFFF) / 4 + 1);
    }

    shadow_record(d, w0);
    d->cp->enqueue_command((unsigned)len * 2, w);
    d->unsynced = true;
    if (id != 0x29) {
        d->dp_after |= d->dp_owed;
        return;
    }
    /* SYNC_FULL: drawn and in RDRAM, then the DP interrupt; or, when the core defers it (bus.defer_dp), the work is
     * only handed to the GPU and dp_finish does the rest at the deadline. */
    dp_finish(d); /* one at a time: an earlier deferred one completes now */
    if (!d->failed && d->bus.defer_dp && d->bus.defer_dp(d->bus.ctx)) {
        on_thread();
        d->dp_timeline = d->cp->signal_timeline();
        d->dp_gpu = true;
        d->dp_owed = true;
        d->dp_after = false;
        d->unsynced = false;
        for (int i = 0; i < 2; i++) {
            d->dp_wr_lo[i] = d->wr_lo[i];
            d->dp_wr_hi[i] = d->wr_hi[i];
            d->wr_lo[i] = d->wr_hi[i] = 0;
        }
        return;
    }
    sync(d);
    d->cp->begin_frame_context(); /* recycles the per-frame Vulkan objects (nothing is in flight now) */
    d->status &= ~(ST_PIPE_BUSY | ST_GCLK);
    if (d->bus.raise)
        d->bus.raise(d->bus.ctx, N64_MI_DP);
}

void fetch64(const VkRdp *d, uint32_t a, uint32_t *hi, uint32_t *lo)
{
    uint8_t b[8];
    if (d->status & ST_XBUS) {
        if (!d->dmem) {
            *hi = *lo = 0;
            return;
        }
        for (int i = 0; i < 8; i++)
            b[i] = d->dmem[(a + (uint32_t)i) & 0xFFF];
    } else {
        for (int i = 0; i < 8; i++) {
            uint32_t x = (a + (uint32_t)i) & 0xFFFFFF;
            b[i] = x < d->bus.rdram_size ? d->bus.rdram[x] : 0;
        }
    }
    *hi = n64_be32(b);
    *lo = n64_be32(b + 4);
}

/* The Qualcomm proprietary driver (Adreno 740 on the AYN Thor) compiles the ubershader and draws check() right, but
 * games come out black (Ocarina of Time: nothing but a few pixels, no Vulkan error): the software RDP instead.
 * RECOMP_N64_RDP_UBERSHADER=1 tries it anyway. */
bool ubershader_draws_wrong(Vulkan::Device *dev)
{
    if (const char *e = getenv("RECOMP_N64_RDP_UBERSHADER"))
        if (atoi(e) != 0)
            return false;
    return dev->get_device_features().driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY_KHR;
}

/* ---- Pipeline cache (n64_vk_cache.h) -------------------------------------------------------------------------------- */
std::mutex g_pc_mu;         /* every save; g_pc_live; the pc_cv waits */
VkRdp *g_pc_live = nullptr; /* the RDP n64_rdp_vk_save_caches saves: from vk_create's success to vk_destroy */
constexpr size_t PC_EMPTY = 16 + 8 + 32; /* Granite's UUID and hash, a Vulkan cache header: nothing compiled in it */

/* The device's VkPipelineCache, from the file when there is a whole one of this device and driver, else empty. Without
 * RECOMP_VK_CACHE_DIR, none (as before). Right after set_context: before any pipeline is made. */
void pc_load(VkRdp *d)
{
    d->pc_path.clear();
    d->pc_size = 0;
    const uint8_t *uuid = d->dev->get_gpu_properties().pipelineCacheUUID;
    char path[1200];
    if (!n64_vk_cache_file(path, sizeof path, uuid))
        return;
    size_t n = 0;
    const char *why = "";
    uint8_t *data = static_cast<uint8_t *>(n64_vk_cache_load(path, uuid, &n, &why));
    if (data && d->dev->init_pipeline_cache(data, n)) {
        d->pc_size = n;
        printf("n64: Vulkan RDP: pipeline cache loaded (%zu KB) from %s\n", n >> 10, path);
    } else {
        if (!data && !strcmp(why, "none"))
            printf("n64: Vulkan RDP: no pipeline cache yet (%s)\n", path);
        else
            fprintf(stderr, "n64: Vulkan RDP: pipeline cache %s ignored (%s), starting empty\n", path,
                    data ? "the driver refused it" : why);
        if (!d->dev->init_pipeline_cache(nullptr, 0)) {
            fprintf(stderr, "n64: Vulkan RDP: the driver cannot make a pipeline cache\n");
            path[0] = 0;
        }
    }
    free(data);
    fflush(stdout);
    d->pc_path = path;
}

/* Writes the cache when it differs in size from what the file holds (new pipelines). g_pc_mu held. */
void pc_save_locked(VkRdp *d, const char *why)
{
    if (d->pc_path.empty() || !d->dev)
        return;
    const size_t n = d->dev->get_pipeline_cache_size();
    if (n == d->pc_size || n == 0)
        return;
    if (n <= PC_EMPTY) {
        if (!d->pc_no_data_logged)
            fprintf(stderr, "n64: Vulkan RDP: the driver gives no pipeline cache data (%zu bytes); only its own "
                            "shader cache (MESA_SHADER_CACHE_DIR) is kept\n", n);
        d->pc_no_data_logged = true;
        return;
    }
    std::vector<uint8_t> buf(n);
    if (!d->dev->get_pipeline_cache_data(buf.data(), n))
        return; /* it grew between the two calls (VK_INCOMPLETE): the next save */
    const uint64_t t0 = host_ns();
    const int rc = n64_vk_cache_save(d->pc_path.c_str(), d->dev->get_gpu_properties().pipelineCacheUUID, buf.data(), n);
    d->pc_size = n; /* a write that failed (logged) is not tried again until the cache grows again */
    if (rc == 0) {
        printf("n64: Vulkan RDP: pipeline cache saved (%zu KB, %s, %.1f ms) to %s\n", n >> 10, why,
               (double)(host_ns() - t0) / 1e6, d->pc_path.c_str());
        fflush(stdout);
    }
}

/* The saver thread: the compiles of the game (a new 4x effect, a new combiner) are on disk within 10 s. */
void pc_saver(VkRdp *d)
{
    std::unique_lock<std::mutex> lk(g_pc_mu);
    while (!d->pc_cv.wait_for(lk, std::chrono::seconds(10), [d]() { return d->pc_quit; }))
        pc_save_locked(d, "new pipelines");
}

void pc_start(VkRdp *d)
{
    {
        std::lock_guard<std::mutex> lk(g_pc_mu);
        g_pc_live = d;
        d->pc_quit = false;
    }
    if (d->pc_path.empty())
        return;
    try {
        d->pc_thread = std::thread(pc_saver, d);
    } catch (...) {
        fprintf(stderr, "n64: Vulkan RDP: no pipeline cache thread; saved at pause and at exit only\n");
    }
}

/* Before the device goes: no more saves from elsewhere, the last one here. */
void pc_stop(VkRdp *d)
{
    {
        std::lock_guard<std::mutex> lk(g_pc_mu);
        if (g_pc_live == d)
            g_pc_live = nullptr;
        d->pc_quit = true;
    }
    d->pc_cv.notify_all();
    if (d->pc_thread.joinable())
        d->pc_thread.join();
    std::lock_guard<std::mutex> lk(g_pc_mu);
    pc_save_locked(d, "exit");
}

/* paraLLEl-RDP's threads (RDP::set_thread_hook) call the hook the runtime gave n64_rdp_set_thread_hook. */
void hook_threads()
{
    n64_rdp_thread_hook fn = nullptr;
    void *user = nullptr;
    n64_rdp_get_thread_hook(&fn, &user);
    RDP::set_thread_hook(fn, user);
}

/* ---- n64_rdp_ops ---------------------------------------------------------------------------------------------------- */
void vk_reset(void *p);

/* "driver <name> <info>, custom <id>" or "..., system", for the start line. */
void driver_label(Vulkan::Device *dev, bool custom, char *out, size_t n)
{
    const Vulkan::DeviceFeatures &f = dev->get_device_features();
    const char *name = f.driver_name[0] ? f.driver_name : "unknown";
    if (custom)
        snprintf(out, n, "driver %s %s, custom %s", name, f.driver_info, n64_vk_driver_id());
    else
        snprintf(out, n, "driver %s %s, system", name, f.driver_info);
}

/* Instance, device and the shader tries on the Vulkan loader init_loader was given last. 1: d->cp is kept for the game;
 * 0: this driver cannot run paraLLEl-RDP (d->ctx / dev / cp reset); custom is 1 for a player's driver (the start line,
 * and the check that adrenotools did not fall back to the device's own driver, which then counts as the system one). */
bool try_driver(VkRdp *d, bool *custom)
{
    const n64_rcp_bus &bus = d->bus;
    d->ctx.reset(new Vulkan::Context);
    Vulkan::Context::SystemHandles handles = {};
    d->ctx->set_system_handles(handles);
    d->ctx->set_num_thread_indices(1);
    if (!d->ctx->init_instance_and_device(nullptr, 0, nullptr, 0, 0)) {
        fprintf(stderr, "n64: Vulkan RDP: no Vulkan device%s\n", *custom ? " on the custom driver" : "");
        d->ctx.reset();
        return false;
    }
    d->dev.reset(new Vulkan::Device);
    d->dev->set_context(*d->ctx);
    pc_load(d); /* before the first command processor: the check's pipelines come from the file too */
    const char *gpu = d->dev->get_gpu_properties().deviceName;
    char drv[2 * VK_MAX_DRIVER_NAME_SIZE + 64];
    if (*custom && d->dev->get_device_features().driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY_KHR) {
        /* adrenotools' hook loads the device's own driver when the custom one fails to load (logcat tag hook_impl) */
        fprintf(stderr, "n64: Vulkan RDP: custom driver %s was not loaded, %s runs on Qualcomm's driver (see logcat "
                        "tag hook_impl)\n", n64_vk_driver_id(), gpu);
        n64_vk_driver_done("not_loaded");
        *custom = false;
    }
    driver_label(d->dev.get(), *custom, drv, sizeof drv);
    d->upscale = fit_upscale(d, n64_rdp_upscale_wanted());
    d->down_steps = scanout_steps(d->upscale);

    /* The default shaders, then without subgroup operations, then the ubershader: the first set that compiles and
     * draws the check right. The device keeps its programs and their failed pipelines from one try to the next. */
    static const struct { RDP::CommandProcessorFlags flags; const char *name; } tries[] = {
        { 0, "default shaders" },
        { RDP::COMMAND_PROCESSOR_FLAG_NO_SUBGROUPS_BIT, "no subgroup operations" },
        { RDP::COMMAND_PROCESSOR_FLAG_NO_SUBGROUPS_BIT | RDP::COMMAND_PROCESSOR_FLAG_UBERSHADER_BIT, "ubershader" },
    };
    bool subgroups = true; /* the last try used subgroup operations */
    for (size_t i = 0; i < sizeof tries / sizeof tries[0]; i++) {
        if (i == 1 && !subgroups)
            continue; /* the same shaders as the default ones */
        if ((tries[i].flags & RDP::COMMAND_PROCESSOR_FLAG_UBERSHADER_BIT) && ubershader_draws_wrong(d->dev.get())) {
            fprintf(stderr, "n64: Vulkan RDP: the ubershader passes the check on %s but draws a black screen in "
                            "games (Ocarina of Time), not used (RECOMP_N64_RDP_UBERSHADER=1 to try it)\n", gpu);
            break;
        }
        d->cp.reset(); /* one set of (upscaled) buffers at a time */
        d->cp.reset(new RDP::CommandProcessor(*d->dev, bus.rdram, 0, bus.rdram_size, bus.rdram_size / 2,
                                              tries[i].flags | upscale_flags(d->upscale)));
        if (!d->cp->device_is_supported() && d->upscale > 1) {
            fprintf(stderr, "n64: Vulkan RDP: upscale %dx cannot start on %s (%s), trying 1x\n", d->upscale, gpu,
                    drv);
            d->upscale = 1;
            d->down_steps = 0;
            i--; /* the same try at 1x (at 0 the unsigned wrap is undone by the loop's i++) */
            continue;
        }
        if (!d->cp->device_is_supported()) {
            fprintf(stderr, "n64: Vulkan RDP: the device (%s, %s) lacks what paraLLEl-RDP needs\n", gpu, drv);
            break;
        }
        subgroups = d->cp->uses_subgroups();
        if (check(d)) {
            d->cp_flags = tries[i].flags | upscale_flags(d->upscale);
            /* A fresh command processor for the game: none of the check's RDP state. */
            d->cp->idle();
            d->cp.reset();
            d->cp.reset(new RDP::CommandProcessor(*d->dev, bus.rdram, 0, bus.rdram_size, bus.rdram_size / 2,
                                                  d->cp_flags));
            set_quirks(d);
            char up[96] = "";
            if (d->upscale > 1)
                snprintf(up, sizeof up, ", upscale %dx, scanout %dx%s", d->upscale, d->upscale >> d->down_steps,
                         (d->cp_flags & RDP::COMMAND_PROCESSOR_FLAG_SUPER_SAMPLED_READ_BACK_BIT)
                             ? ", super-sampled readback" : "");
            printf("n64: Vulkan RDP on %s (paraLLEl-RDP, %s; %s%s)\n", gpu, tries[i].name, drv, up);
            fflush(stdout);
            std::lock_guard<std::mutex> lk(g_pc_mu);
            pc_save_locked(d, "startup check");
            return true;
        }
        if (d->upscale > 1) {
            fprintf(stderr, "n64: Vulkan RDP: %s fail upscaled (%dx) on %s, trying 1x\n", tries[i].name, d->upscale,
                    gpu);
            d->cp->idle();
            d->upscale = 1;
            d->down_steps = 0;
            i--; /* the same try at 1x */
            continue;
        }
        fprintf(stderr, "n64: Vulkan RDP: %s fail on %s%s\n", tries[i].name, gpu,
                i + 1 < sizeof tries / sizeof tries[0] ? ", trying again" : "");
        d->cp->idle();
    }
    if (d->cp && d->cp->device_is_supported())
        fprintf(stderr, "n64: Vulkan RDP: the driver of %s (%s) cannot run paraLLEl-RDP's shaders\n", gpu, drv);
    d->cp.reset(); /* the command processor, then the device, then the instance */
    d->dev.reset();
    d->ctx.reset();
    d->pc_path.clear();
    return false;
}

/* The loaders, in order: the player's driver (n64_vk_driver.h) when one is selected and opens, then the system's.
 * Custom driver -> system driver -> NULL, and n64_rdp_select.c starts the software RDP; each step is logged. */
void *vk_create(n64_rcp_bus bus)
{
    /* paraLLEl-RDP's incoherent path pages RDRAM by a power of two. */
    if (!bus.rdram || bus.rdram_size < (1u << 20) || (bus.rdram_size & (bus.rdram_size - 1)))
        return nullptr;
    on_thread();
    std::unique_ptr<VkRdp> d(new (std::nothrow) VkRdp);
    if (!d)
        return nullptr;
    d->bus = bus;
    Vulkan::get_stalled_compiles(d->stalls0, d->stall_ns0); /* the check's compiles count as this RDP's */
    n64_vk_cache_mesa_env(); /* before any driver opens */
    hook_threads();          /* before any command processor starts its threads */

    if (void *gipa = n64_vk_driver_open()) {
        bool custom = true;
        /* vkCreateInstance resolved: a loader that answers nothing would be called through a null pointer */
        if (Vulkan::Context::init_loader(reinterpret_cast<PFN_vkGetInstanceProcAddr>(gipa)) && vkCreateInstance) {
            const bool ok = try_driver(d.get(), &custom);
            if (custom) /* opened and made a loader: what fails now is paraLLEl-RDP on this driver */
                n64_vk_driver_done(ok ? "ok" : "unsupported");
            if (ok) {
                vk_reset(d.get());
                pc_start(d.get());
                return d.release();
            }
        } else {
            n64_vk_driver_done("open_failed");
        }
        if (!custom) {
            fprintf(stderr, "n64: Vulkan RDP: the device's own driver was already tried\n");
            return nullptr; /* adrenotools fell back to it: the same driver again would fail the same way */
        }
        fprintf(stderr, "n64: Vulkan RDP: custom driver %s cannot run paraLLEl-RDP, trying the system driver\n",
                n64_vk_driver_id());
        /* the system's libvulkan again, volk re-initialised (the adrenotools handle stays loaded) */
        if (!Vulkan::Context::init_loader(nullptr, true)) {
            fprintf(stderr, "n64: Vulkan RDP: no Vulkan loader\n");
            return nullptr;
        }
    } else if (!Vulkan::Context::init_loader(nullptr)) {
        fprintf(stderr, "n64: Vulkan RDP: no Vulkan loader\n");
        return nullptr;
    }
    bool custom = false;
    if (!try_driver(d.get(), &custom))
        return nullptr;
    vk_reset(d.get());
    pc_start(d.get());
    return d.release();
}

void vk_destroy(void *p)
{
    VkRdp *d = static_cast<VkRdp *>(p);
    if (!d)
        return;
    on_thread();
    if (d->cp)
        d->cp->idle();
    pc_stop(d); /* the compiles are over: the last save */
    d->pf[0] = RDP::VIScanoutBuffer(); /* Vulkan objects: before their device */
    d->pf[1] = RDP::VIScanoutBuffer();
    d->cp.reset(); /* the command processor, then the device, then the instance */
    d->dev.reset();
    d->ctx.reset();
    delete d;
}

void vk_reset(void *p)
{
    VkRdp *d = static_cast<VkRdp *>(p);
    sync(d);
    d->dp_owed = d->dp_after = false; /* the machine's reset dropped its deadline */
    d->pf_valid = false;
    d->start = d->end = d->current = 0;
    d->status = 0;
    d->clock = d->bufbusy = d->pipebusy = d->tmemc = 0;
    d->start_pending = 0;
    d->npend = 0;
    d->fe_cycle = 0;
    d->fe_ci_size = 2;
    d->fe_ci_width = 320;
    d->fe_ci_addr = d->fe_zi_addr = 0;
    d->fe_sc_y0 = 0;
    d->fe_sc_y1 = 0xFFF;
    d->fe_zon = 0;
    d->wr_lo[0] = d->wr_hi[0] = d->wr_lo[1] = d->wr_hi[1] = 0;
    d->shadow_valid = 0;
    d->shadow_tiles = d->shadow_sizes = 0;
}

void vk_attach_dmem(void *p, const uint8_t *dmem) { static_cast<VkRdp *>(p)->dmem = dmem; }

uint32_t vk_read_reg(void *p, uint32_t reg)
{
    VkRdp *d = static_cast<VkRdp *>(p);
    switch (reg & 7) {
    case 0: return d->start;
    case 1: return d->end;
    case 2: return d->current;
    case 3: {
        uint32_t st = d->status | ST_CBUF_READY;
        if (d->current != d->end || d->npend)
            st |= ST_CMD_BUSY | ST_DMA_BUSY;
        if (d->start_pending)
            st |= ST_START_PENDING;
        return st;
    }
    case 4: return d->clock;
    case 5: return d->bufbusy;
    case 6: return d->pipebusy;
    default: return d->tmemc;
    }
}

void vk_write_reg(void *p, uint32_t reg, uint32_t v)
{
    VkRdp *d = static_cast<VkRdp *>(p);
    switch (reg & 7) {
    case 0:
        if (!d->start_pending) {
            d->start = v & 0xFFFFF8;
            d->start_pending = 1;
        }
        break;
    case 1:
        d->end = v & 0xFFFFF8;
        d->status |= ST_PIPE_BUSY | ST_GCLK;
        if (d->start_pending) {
            d->current = d->start;
            d->start_pending = 0;
            d->npend = 0;
        }
        break;
    case 3:
        if (v & 0x001) d->status &= ~ST_XBUS;
        if (v & 0x002) d->status |= ST_XBUS;
        if (v & 0x004) d->status &= ~ST_FREEZE;
        if (v & 0x008) d->status |= ST_FREEZE;
        if (v & 0x010) d->status &= ~ST_FLUSH;
        if (v & 0x020) d->status |= ST_FLUSH;
        if (v & 0x040) d->tmemc = 0;
        if (v & 0x080) d->pipebusy = 0;
        if (v & 0x100) d->bufbusy = 0;
        if (v & 0x200) d->clock = 0;
        break;
    default:
        break;
    }
}

void vk_run(void *p)
{
    VkRdp *d = static_cast<VkRdp *>(p);
    if (d->status & ST_FREEZE)
        return;
    if (d->current < d->end)
        on_thread();
    while (d->current < d->end) {
        fetch64(d, d->current, &d->pend[2 * d->npend], &d->pend[2 * d->npend + 1]);
        d->npend++;
        d->current += 8;
        int need = cmd_len((int)(d->pend[0] >> 24) & 0x3F);
        if (d->npend >= need) {
            dispatch(d, d->pend, need);
            d->npend = 0;
        }
    }
}

int vk_set_threads(void *p, int n)
{
    (void)p;
    (void)n;
    return 1;
}

void vk_flush(void *p)
{
    VkRdp *d = static_cast<VkRdp *>(p);
    sync(d);
}

int vk_failed(void *p) { return static_cast<VkRdp *>(p)->failed; }

int vk_upscale(void *p) { return static_cast<VkRdp *>(p)->upscale; }

int vk_scanout_scale(void *p)
{
    const VkRdp *d = static_cast<VkRdp *>(p);
    return d->upscale > 1 ? d->upscale >> d->down_steps : 0;
}

/* The prefetched scanout is of these registers and field, and nothing was drawn where it reads since. */
bool pf_matches(const VkRdp *d, const uint32_t *vi, uint32_t field)
{
    if (!d->pf_valid || d->pf_dirty || field != d->pf_field)
        return false;
    for (int r = 0; r < 14; r++)
        if (r != 4 && vi[r] != d->pf_vi[r]) /* V_CURRENT: the field stands for it */
            return false;
    return true;
}

/* n64_rdp.h n64_rdp_scanout_prefetch: the scanout started now, waited for at n64_display. Only when nothing is queued
 * since the last wait: then nothing is in front of it on the GPU and the command ring is empty. */
void vk_scanout_prefetch(void *p, const uint32_t *vi, uint32_t field, uint32_t lo, uint32_t hi)
{
    VkRdp *d = static_cast<VkRdp *>(p);
    d->pf_valid = false;
    if (d->upscale <= 1 || d->failed || d->unsynced || d->dp_gpu)
        return;
    on_thread();
    for (unsigned r = 0; r < unsigned(RDP::VIRegister::Count); r++)
        d->cp->set_vi_register(RDP::VIRegister(r), r == unsigned(RDP::VIRegister::VCurrentLine) ? field : vi[r]);
    RDP::ScanoutOptions so;
    so.downscale_steps = d->down_steps;
    d->pf_cur ^= 1;
    d->cp->scanout_async_buffer(d->pf[d->pf_cur], so);
    if (!d->pf[d->pf_cur].fence)
        return; /* it shows nothing: the scanout at n64_display says so */
    memcpy(d->pf_vi, vi, sizeof d->pf_vi);
    d->pf_field = field;
    d->pf_lo = lo;
    d->pf_hi = hi;
    d->pf_dirty = false;
    d->pf_valid = true;
    d->stats.prefetches++;
}

void vk_scanout_forget(void *p) { static_cast<VkRdp *>(p)->pf_valid = false; }

/* The prefetched picture, waited for (a GPU wait, counted as a scanout's). */
const RDP::RGBA *pf_take(VkRdp *d, unsigned *sw, unsigned *sh)
{
    RDP::VIScanoutBuffer &b = d->pf[d->pf_cur];
    const uint64_t t0 = host_ns();
    b.fence->wait();
    const uint64_t dt = host_ns() - t0;
    d->stats.waits++;
    d->stats.wait_ns += dt;
    d->stats.scanouts++;
    d->stats.scanout_ns += dt;
    d->stats.prefetch_hits++;
    d->pf_valid = false;
    *sw = b.width;
    *sh = b.height;
    return static_cast<const RDP::RGBA *>(d->dev->map_host_buffer(*b.buffer, Vulkan::MEMORY_ACCESS_READ_BIT));
}

/* n64_rdp.h n64_rdp_scanout_rgba: the picture in one of the two kept buffers, read where the GPU copied it (mapped,
 * no copy): the prefetched one when it is of this picture (n64_rdp_scanout_prefetch), else a scanout now, after
 * everything queued is drawn. */
int vk_scanout_rgba(void *p, const uint32_t *vi, uint32_t field, const uint8_t **rgba, int *w, int *h, int *pitch)
{
    VkRdp *d = static_cast<VkRdp *>(p);
    if (d->upscale <= 1 || d->failed)
        return -1;
    on_thread();
    unsigned sw = 0, sh = 0;
    const RDP::RGBA *src;
    if (pf_matches(d, vi, field)) {
        src = pf_take(d, &sw, &sh);
    } else {
        d->pf_valid = false;
        sync(d);
        for (unsigned r = 0; r < unsigned(RDP::VIRegister::Count); r++)
            d->cp->set_vi_register(RDP::VIRegister(r), r == unsigned(RDP::VIRegister::VCurrentLine) ? field : vi[r]);
        RDP::ScanoutOptions so;
        so.downscale_steps = d->down_steps;
        const uint64_t t0 = host_ns();
        d->pf_cur ^= 1; /* the other buffer: the last picture handed out stays readable */
        RDP::VIScanoutBuffer &b = d->pf[d->pf_cur];
        d->cp->scanout_async_buffer(b, so);
        if (b.fence)
            b.fence->wait();
        const uint64_t dt = host_ns() - t0;
        d->stats.waits++;
        d->stats.wait_ns += dt;
        d->stats.scanouts++;
        d->stats.scanout_ns += dt;
        d->cp->begin_frame_context(); /* the scanout waited: nothing of this frame is in flight */
        if (!b.fence || !b.width || !b.height)
            return -1;
        sw = b.width;
        sh = b.height;
        src = static_cast<const RDP::RGBA *>(d->dev->map_host_buffer(*b.buffer, Vulkan::MEMORY_ACCESS_READ_BIT));
    }
    if (!src || !sw || !sh)
        return -1;
    *rgba = reinterpret_cast<const uint8_t *>(src); /* RDP::RGBA: r, g, b, a bytes */
    *w = (int)sw;
    *h = (int)sh;
    *pitch = (int)sw * 4;
    return 0;
}

/* n64_display's picture at the upscaled size (n64_rdp.h n64_rdp_scanout) as 0x00RRGGBB: vk_scanout_rgba converted. */
int vk_scanout(void *p, const uint32_t *vi, uint32_t field, uint32_t *xrgb, int max_w, int max_h, int *w, int *h)
{
    const uint8_t *src = nullptr;
    int sw = 0, sh = 0, pitch = 0;
    if (vk_scanout_rgba(p, vi, field, &src, &sw, &sh, &pitch) != 0 || sw > max_w || sh > max_h)
        return -1;
    for (int y = 0; y < sh; y++) {
        const uint8_t *s = src + (size_t)y * (size_t)pitch;
        uint32_t *o = xrgb + (size_t)y * (size_t)sw;
        for (int x = 0; x < sw; x++)
            o[x] = (uint32_t)s[4 * x] << 16 | (uint32_t)s[4 * x + 1] << 8 | s[4 * x + 2];
    }
    *w = sw;
    *h = sh;
    return 0;
}

/* ---- Save states (XBA-183, n64_rdp_backend.h) ---------------------------------------------------------------------- */
/* Drawn out and synchronised before a save (RDRAM holds every pixel); the extra bytes are the shadow of state commands.
 * On load: synchronised, the front end's registers set, the shadow replayed into paraLLEl-RDP (its modes, combiner,
 * colours, images, scissor and tiles are the state's; TMEM is whatever it held). The upscaled copy of RDRAM
 * (XBA-185) is derived: paraLLEl-RDP compares RDRAM with its reference copy before each pass and takes the 1x pixels
 * where they changed, so the upscaled frame buffers follow the loaded RDRAM from the next frame drawn. */
struct VkShadow {
    uint64_t cmd[0x40], tile[8], size[8], valid;
    uint32_t tiles, sizes;
};

size_t vk_state_extra_size(void *p)
{
    (void)p;
    return sizeof(VkShadow);
}

void vk_state_save(void *p, n64_rdp_common_state *c, uint8_t *extra)
{
    VkRdp *d = static_cast<VkRdp *>(p);
    on_thread();
    sync(d); /* (a deferred DP interrupt: the core completes it before a save, n64_state_save) */
    memset(c, 0, sizeof *c);
    c->start = d->start;
    c->end = d->end;
    c->current = d->current;
    c->status = d->status;
    c->clock = d->clock;
    c->bufbusy = d->bufbusy;
    c->pipebusy = d->pipebusy;
    c->tmemc = d->tmemc;
    c->start_pending = (uint32_t)d->start_pending;
    c->npend = (uint32_t)d->npend;
    for (int i = 0; i < d->npend && i < 22; i++)
        c->pend[i] = (uint64_t)d->pend[2 * i] << 32 | d->pend[2 * i + 1];
    c->fe_cycle = d->fe_cycle;
    c->fe_ci_size = d->fe_ci_size;
    c->fe_ci_width = d->fe_ci_width;
    c->fe_zon = d->fe_zon;
    c->fe_sc_y0 = d->fe_sc_y0;
    c->fe_sc_y1 = d->fe_sc_y1;
    c->fe_ci_addr = d->fe_ci_addr;
    c->fe_zi_addr = d->fe_zi_addr;
    c->fe_ti_size = 0;
    c->fe_ti_width = 1;
    VkShadow sh{};
    memcpy(sh.cmd, d->shadow, sizeof sh.cmd);
    memcpy(sh.tile, d->shadow_tile, sizeof sh.tile);
    memcpy(sh.size, d->shadow_size, sizeof sh.size);
    sh.valid = d->shadow_valid;
    sh.tiles = d->shadow_tiles;
    sh.sizes = d->shadow_sizes;
    memcpy(extra, &sh, sizeof sh);
}

int vk_state_load(void *p, const n64_rdp_common_state *c, const uint8_t *extra, size_t n)
{
    VkRdp *d = static_cast<VkRdp *>(p);
    if ((extra && n != sizeof(VkShadow)) || c->npend > 22)
        return -1;
    on_thread();
    sync(d);
    d->dp_owed = d->dp_after = false; /* the machine's events are the state's (none pending) */
    d->pf_valid = false;
    d->start = c->start;
    d->end = c->end;
    d->current = c->current;
    d->status = c->status;
    d->clock = c->clock;
    d->bufbusy = c->bufbusy;
    d->pipebusy = c->pipebusy;
    d->tmemc = c->tmemc;
    d->start_pending = (int)c->start_pending;
    d->npend = (int)c->npend;
    for (int i = 0; i < d->npend; i++) {
        d->pend[2 * i] = (uint32_t)(c->pend[i] >> 32);
        d->pend[2 * i + 1] = (uint32_t)c->pend[i];
    }
    d->fe_cycle = c->fe_cycle;
    d->fe_ci_size = c->fe_ci_size;
    d->fe_ci_width = c->fe_ci_width;
    d->fe_zon = c->fe_zon;
    d->fe_sc_y0 = c->fe_sc_y0;
    d->fe_sc_y1 = c->fe_sc_y1;
    d->fe_ci_addr = c->fe_ci_addr;
    d->fe_zi_addr = c->fe_zi_addr;
    d->wr_lo[0] = d->wr_hi[0] = d->wr_lo[1] = d->wr_hi[1] = 0;
    if (!extra)
        return 0;
    VkShadow sh;
    memcpy(&sh, extra, sizeof sh);
    memcpy(d->shadow, sh.cmd, sizeof d->shadow);
    memcpy(d->shadow_tile, sh.tile, sizeof d->shadow_tile);
    memcpy(d->shadow_size, sh.size, sizeof d->shadow_size);
    d->shadow_valid = sh.valid;
    d->shadow_tiles = sh.tiles;
    d->shadow_sizes = sh.sizes;
    if (d->failed)
        return 0;
    auto put1 = [d](uint64_t w) {
        const uint32_t words[2] = { (uint32_t)(w >> 32), (uint32_t)w };
        d->cp->enqueue_command(2, words);
        d->unsynced = true;
    };
    for (int id = 0; id < 0x40; id++)
        if (sh.valid >> id & 1)
            put1(sh.cmd[id]);
    for (int t = 0; t < 8; t++) {
        if (sh.tiles >> t & 1)
            put1(sh.tile[t]);
        if (sh.sizes >> t & 1)
            put1(sh.size[t]);
    }
    sync(d);
    return 0;
}

void vk_stats(void *p, n64_rdp_stats *out)
{
    const VkRdp *d = static_cast<VkRdp *>(p);
    *out = d->stats;
    if (d->cp)
        d->cp->get_ring_blocked(out->queue_waits, out->queue_ns);
    uint64_t n = 0, ns = 0;
    Vulkan::get_stalled_compiles(n, ns);
    out->stalled_compiles = n - d->stalls0;
    out->stalled_compile_ns = ns - d->stall_ns0;
}

/* n64_rdp_unreported: the rows queued since the last wait (wrote_add's ranges, reported at the next sync) against
 * RDRAM [lo, hi). */
int vk_unreported(void *p, uint32_t lo, uint32_t hi)
{
    const VkRdp *d = static_cast<VkRdp *>(p);
    for (int i = 0; i < 2; i++) {
        if (d->wr_hi[i] > d->wr_lo[i] && d->wr_lo[i] < hi && lo < d->wr_hi[i])
            return 1;
        if (d->dp_wr_hi[i] > d->dp_wr_lo[i] && d->dp_wr_lo[i] < hi && lo < d->dp_wr_hi[i])
            return 1;
    }
    return 0;
}

/* n64_rdp.h n64_rdp_dp_complete. */
void vk_dp_complete(void *p) { dp_finish(static_cast<VkRdp *>(p)); }

} // namespace

extern "C" void n64_rdp_vk_save_caches(const char *why)
{
    std::lock_guard<std::mutex> lk(g_pc_mu);
    if (g_pc_live)
        pc_save_locked(g_pc_live, why);
}

extern "C" const n64_rdp_ops n64_rdp_vk_ops = { "vulkan", vk_create, vk_destroy, vk_reset, vk_attach_dmem,
                                                vk_read_reg, vk_write_reg, vk_run, vk_set_threads, vk_flush,
                                                vk_failed, vk_upscale, vk_scanout_scale, vk_scanout,
                                                vk_state_extra_size, vk_state_save, vk_state_load,
                                                vk_stats, vk_unreported, vk_dp_complete, vk_scanout_prefetch,
                                                vk_scanout_forget, vk_scanout_rgba };
