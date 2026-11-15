# paraLLEl-RDP (vendored)

The Nintendo 64 RDP on Vulkan compute, by Themaister: an LLE renderer that aims to match the angrylion reference
renderer bit for bit. Upstream's own README is `README.upstream.md`. The N64 runtime uses it as its GPU RDP
(`consoles/n64/runtime/n64_rdp_vk.cpp`, Plane XBA-173). It is selected at run time with `RECOMP_N64_RDP`
(`consoles/README.md`), and the software RDP `n64_rdp.c` is always the fallback.

## Source

- <https://github.com/Themaister/parallel-rdp-standalone> at `388d70f5835b352d841d9d9e5a08c5de01470f41`
  (2024-11-09). This is upstream's tree for embedding in emulators: paraLLEl-RDP plus the parts of Granite it needs,
  volk, the Vulkan headers and the SPIR-V shaders precompiled into `parallel-rdp/shaders/slangmosh.hpp`. It was
  generated from Themaister/parallel-rdp `1cecd042b2619bc505c12bfdc713808386f2b54d` (`COMMIT`).
- Downloaded as a tarball from codeload.github.com.

## Licences (no GPL code)

| Directory | Licence | Text |
|---|---|---|
| `parallel-rdp/` | MIT, Copyright (c) 2020 Themaister | `LICENSE` |
| `vulkan/`, `util/` (Granite) | MIT, Copyright (c) 2017-2023 Hans-Kristian Arntzen | each file's header, `vulkan/LICENSE.Granite` |
| `volk/` | MIT, Copyright (c) 2018-2023 Arseny Kapoulkine | end of `volk/volk.h` |
| `vulkan-headers/` (1.3.278) | Apache-2.0, The Khronos Group | `vulkan-headers/LICENSE.md`, `vulkan-headers/LICENSE.Apache-2.0.txt` |

paraLLEl-RDP studies angrylion (GPL) but contains none of its code (upstream README, « Disclaimer »). Its conformance
suite and the angrylion submodule are not part of the standalone tree and are not vendored.

## Trimmed

The following were removed from the standalone tree:

- `vulkan/wsi.*`, `vulkan/device_fossilize.*` and `vulkan/renderdoc_capture.cpp`: not compiled (upstream's
  `config.mk` leaves them out too).
- The platform headers we don't use from `vulkan-headers/include/vulkan/`. What remains: `vk_platform.h`,
  `vulkan.h`, `vulkan_core.h`, `vulkan_android.h` and `vk_video/`.
- `config.mk`, replaced by `CMakeLists.txt` (one static library, `parallel_rdp`).

## Patches (ours, marked `xboxrecomp-android patch`)

Our RDRAM is the core's byte array in the N64's byte order (`n64_devices.h`). paraLLEl-RDP expects 32-bit host
words. With `PARALLEL_RDP_HOST_RDRAM_BSWAP32` (set by `CMakeLists.txt`):

- `parallel-rdp/rdp_common.hpp`: `host_rdram_copy_to_gpu`, a copy that byte-swaps each 32-bit word.
- `parallel-rdp/rdp_renderer.cpp`: the three host-to-GPU page copies of the incoherent path (`memcpy` into the
  mapped GPU RDRAM or the staging buffer) use it.
- `parallel-rdp/rdp_device.cpp`: `masked_memcpy` swaps the GPU's words and write masks on the way back. Imported
  host memory (`VK_EXT_external_memory_host`) is never used: the shaders would read our bytes in the wrong order.

Pipelines a driver cannot compile (the Adreno 740 of the AYN Thor failed one compute pipeline at every dispatch,
~6 ms each, and dropped the dispatch: black screen at 15 fps):

- `vulkan/shader.hpp`, `vulkan/shader.cpp`: `Program` remembers the hashes of its pipelines that failed
  (`pipeline_failed`, `mark_pipeline_failed`), and whether one failed in a synchronous compile, where nothing
  replaces it (`has_mandatory_failure`). A specialised rasterizer variant fails in the background thread and the
  generic rasterizer is used instead: that one does not count. `set_debug_name` names the program for the log.
- `vulkan/command_buffer.cpp`: `build_compute_pipeline` returns at once for a pipeline that already failed, and on a
  failure logs the program's name, the shader hash, the spec constants and the subgroup size control, once.
  `dispatch` and `dispatch_indirect` log the first 16 dropped dispatches only. `PARALLEL_RDP_TEST_FAIL_PROGRAM`
  (program names, comma-separated) makes those programs fail as such a driver would, to test the fallbacks.
- `parallel-rdp/rdp_renderer.*`: `set_shader_bank` names the bank's programs. `has_pipeline_failure` tells whether
  a program these caps use failed (the ubershader, or the rasterizer and depth_blend; the upscaling programs only
  when upscaling, so a device whose upscaled pipelines failed can still be checked at 1x, XBA-185). `uses_subgroups` and
  `uses_ubershader` report the caps. `RendererOptions::disable_subgroups` and `force_ubershader` do what
  `PARALLEL_RDP_SUBGROUP=0` and `PARALLEL_RDP_UBERSHADER=1` do. `init_caps` logs the caps it picked (driver ID,
  subgroup size, ubershader, small types, subgroups).
- `parallel-rdp/rdp_device.*`: `COMMAND_PROCESSOR_FLAG_NO_SUBGROUPS_BIT` and `COMMAND_PROCESSOR_FLAG_UBERSHADER_BIT`
  feed those options, and `CommandProcessor` forwards the three queries.

The driver's name and version (`driverName`, `driverInfo` of `VkPhysicalDeviceDriverProperties`, for the start line,
which must say whether a custom driver such as Turnip is in use):

- `vulkan/context.hpp`, `vulkan/context.cpp`: `DeviceFeatures` keeps `driver_name` and `driver_info` next to
  `driver_id`.

The N64 runtime uses them (`consoles/n64/runtime/n64_rdp_vk.cpp`): a check at start, then default shaders, no
subgroups, ubershader, else the software RDP (`consoles/README.md`).

Counters for the N64 runtime's performance line (`n64: perf`, `consoles/README.md`), host-only, no effect on drawing:

- `parallel-rdp/command_ring.*`: `enqueue_command` times its waits for room in a full ring (`blocked_waits`,
  `blocked_ns`; no clock read when there is room); `parallel-rdp/rdp_device.*`: `CommandProcessor::get_ring_blocked`.
- `vulkan/command_buffer.*`: `get_stalled_compiles`, the count and total time of the "Stalled compile" pipelines.

Threads in the host's performance session (the N64 runtime's `n64_rdp_set_thread_hook`, which also covers the
software RDP's band threads):

- `parallel-rdp/thread_hook.hpp` (new), `parallel-rdp/command_ring.cpp`: `set_thread_hook` keeps a process-wide hook
  and `ThreadHookScope` calls it on its own thread at start (1) and end (0).
- `parallel-rdp/command_ring.cpp` (`CommandRing::thread_loop`, the command ring worker) and
  `parallel-rdp/worker_thread.hpp` (`WorkerThread::main_loop`: the fence executor and the pipeline compile worker)
  hold a `ThreadHookScope` for their whole loop.

The command ring (the N64 runtime's deferred DP interrupt, `consoles/n64/runtime/n64_core.c`):

- `parallel-rdp/rdp_device.cpp`: `ring.init(..., 1u << 18)`, 1 MB of commands instead of 16 KB (about 100
  triangles), so the emulation thread does not wait for the GPU while a frame's display list is queued.

The pipeline cache is unchanged Granite: `Device::init_pipeline_cache(data, size)`, `get_pipeline_cache_size` and
`get_pipeline_cache_data`, called by the N64 runtime (`n64_vk_cache.h`), as this tree has no filesystem for Granite's
own `cache://pipeline_cache.bin`.

The shaders are unchanged, so the precompiled SPIR-V is upstream's.

## Symbols

`CMakeLists.txt` generates `prdp_volk_rename.h` from `volk/volk.h` and forces it into every file of the library and
of its users. It renames volk's globals (`vkCreateInstance` & co., `volkInitialize`, ...) to `prdp_*`. Another volk
can then live in the same `libmain.so`, such as the LSFG bridge's (`src/framegen/lsfg_vulkan.cmake`). Vulkan is
dlopened at run time (`libvulkan.so.1`, or `libvulkan.so` on Android), never linked.

## Updating

1. Take a newer `parallel-rdp-standalone` commit.
2. Copy the same directories and trim as above.
3. Re-apply the patches (grep `xboxrecomp-android patch`).
4. Run `n64_rdp_test` with `RECOMP_N64_RDP=vulkan`, and `n64_rdp_test --compare`
   (`consoles/n64/tests/CMakeLists.txt`).
