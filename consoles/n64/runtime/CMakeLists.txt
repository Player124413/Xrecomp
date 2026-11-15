cmake_minimum_required(VERSION 3.20)
project(n64_runtime C)

# n64_runtime: the Nintendo 64 runtime (MIPS VR4300, big-endian, 64-bit), XBA-146. The core (N-1 XBA-169 ROM / boot,
# N-3 XBA-171 interpreter, N-4 XBA-172 PI / SI / VI / AI / saves) is n64_core.h; the RSP and the RDP are reached
# through n64_devices.h. See consoles/README.md and the Plane spec « Spec · Other consoles ». Built when
# XBR_CONSOLE_N64 is ON (root CMakeLists.txt); a project may also add this directory alone (consoles/n64/tests does).
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# macOS: _POSIX_C_SOURCE hides the BSD / C99 extras (snprintf, mkdtemp...) unless _DARWIN_C_SOURCE is set.
if(APPLE)
    add_compile_definitions(_DARWIN_C_SOURCE)
endif()

# The RCP devices, each the real module (n64_rsp.c / n64_rdp.c) when present or its placeholder n64_<dev>_stub.c, in a
# target of its own so the modules merge without duplicate symbols.
# n64_rdp_sw: the software RDP (N-5, XBA-173, n64_devices.h + n64_rdp.h): plain C11 (+ pthreads for the optional
# scanline bands), no host dependency, so tests build anywhere. n64_rdp.c replaces the core's n64_rdp_stub.c when it is
# present. Its public functions are renamed n64_rdp_sw_* (n64_rdp_sw_names.h, forced in): n64_rdp below dispatches.
if(NOT TARGET n64_rdp_sw AND (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/n64_rdp.c OR EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/n64_rdp_stub.c))
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/n64_rdp.c)
        set(N64_RDP_SRC n64_rdp.c)
    else()
        set(N64_RDP_SRC n64_rdp_stub.c)
    endif()
    add_library(n64_rdp_sw STATIC ${N64_RDP_SRC})
    target_include_directories(n64_rdp_sw PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(n64_rdp_sw PRIVATE -Wall -Wextra)
    if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(n64_rdp_sw PRIVATE -O2) # the software rasterizer is far too slow unoptimised
    endif()
    # Auto-vectorised 4-channel code reads back scalar stores as vectors (store-forwarding stalls): 1.6x slower.
    target_compile_options(n64_rdp_sw PRIVATE $<$<C_COMPILER_ID:GNU>:-fno-tree-vectorize>
                           $<$<C_COMPILER_ID:Clang,AppleClang>:-fno-slp-vectorize>)
    set_target_properties(n64_rdp_sw PROPERTIES POSITION_INDEPENDENT_CODE ON)
    find_package(Threads)
    if(Threads_FOUND)
        target_link_libraries(n64_rdp_sw PUBLIC Threads::Threads)
    endif()
    target_compile_options(n64_rdp_sw PRIVATE "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/n64_rdp_sw_names.h")
endif()

# n64_rdp: the RDP the core links (n64_devices.h + n64_rdp.h), n64_rdp_select.c choosing a back end at n64_rdp_create
# (n64_rdp_backend.h): the software RDP, or with XBR_N64_RDP_VULKAN the Vulkan one, n64_rdp_vk.cpp over paraLLEl-RDP
# (third_party/parallel-rdp, MIT; C++17; Vulkan loaded at run time, never linked). ON by default on Android (the AYN
# Thor's Adreno), OFF elsewhere: a desktop build then needs no C++ (-DXBR_N64_RDP_VULKAN=ON to test it, with Mesa's
# lavapipe on a machine without a GPU). A project that links n64_rdp with it ON enables CXX in its own top directory
# (consoles/n64/tests and the Android app do).
get_filename_component(N64_PRDP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/parallel-rdp ABSOLUTE)
if(ANDROID AND EXISTS ${N64_PRDP_DIR}/CMakeLists.txt)
    set(_n64_vk_default ON)
else()
    set(_n64_vk_default OFF)
endif()
option(XBR_N64_RDP_VULKAN "N64: link the Vulkan RDP (paraLLEl-RDP) next to the software RDP" ${_n64_vk_default})
if(NOT TARGET n64_rdp AND TARGET n64_rdp_sw)
    add_library(n64_rdp STATIC n64_rdp_select.c)
    target_include_directories(n64_rdp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(n64_rdp PRIVATE -Wall -Wextra)
    target_link_libraries(n64_rdp PUBLIC n64_rdp_sw)
    set_target_properties(n64_rdp PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(XBR_N64_RDP_VULKAN)
        enable_language(CXX)
        add_subdirectory(${N64_PRDP_DIR} ${CMAKE_CURRENT_BINARY_DIR}/parallel-rdp)
        # n64_vk_driver.c: a driver the player supplied (Turnip...) through libxba_vkdriver.so, dlopened only when
        # RECOMP_VK_DRIVER_DIR selects one (n64_vk_driver.h); off Android that library is absent and the open fails.
        # n64_vk_cache.c: the pipeline cache file under RECOMP_VK_CACHE_DIR (n64_vk_cache.h).
        add_library(n64_rdp_vk STATIC n64_rdp_vk.cpp n64_vk_driver.c n64_vk_cache.c)
        target_include_directories(n64_rdp_vk PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_definitions(n64_rdp_vk PUBLIC N64_RDP_HAVE_VULKAN)
        target_compile_options(n64_rdp_vk PRIVATE -Wall -Wextra)
        target_link_libraries(n64_rdp_vk PRIVATE parallel_rdp ${CMAKE_DL_LIBS})
        set_target_properties(n64_rdp_vk PROPERTIES POSITION_INDEPENDENT_CODE ON)
        target_link_libraries(n64_rdp PUBLIC n64_rdp_vk)
    endif()
endif()

# n64_rsp: the RSP (N-5, XBA-173, n64_devices.h), LLE scalar + vector unit: plain C11, no host dependency, so tests
# build anywhere. n64_rsp.c replaces the core's n64_rsp_stub.c when it is present. It calls the n64_rdp_* functions
# (COP0 8..15) but does not link an RDP: the consumer links one (n64_rdp, or a test's fake). Vectorisation stays on:
# the 8-lane vector-unit loops are 1.7x faster with it.
if(NOT TARGET n64_rsp AND (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/n64_rsp.c OR EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/n64_rsp_stub.c))
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/n64_rsp.c)
        set(N64_RSP_SRC n64_rsp.c)
    else()
        set(N64_RSP_SRC n64_rsp_stub.c)
    endif()
    add_library(n64_rsp STATIC ${N64_RSP_SRC})
    target_include_directories(n64_rsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(n64_rsp PRIVATE -Wall -Wextra)
    if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(n64_rsp PRIVATE -O2) # microcode must run in real time even in debug builds
    endif()
    set_target_properties(n64_rsp PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# n64_core: the emulated machine (CPU, COP0 / TLB, COP1, bus, MI / PI / SI / PIF / VI / AI, saves) over n64_rsp and
# n64_rdp. Plain C11 + libm, no host dependency, so the tests and n64_run build anywhere (no SDL).
if(NOT TARGET n64_core)
    add_library(n64_core STATIC n64_core.c n64_cpu.c n64_fpu.c n64_bus.c n64_io.c n64_vi.c n64_pif.c n64_rom.c n64_recomp.c
                n64_state.c)
    target_include_directories(n64_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(n64_core PUBLIC n64_rsp n64_rdp)
    find_library(N64_LIBM m)
    if(N64_LIBM)
        target_link_libraries(n64_core PUBLIC ${N64_LIBM})
    endif()
    target_compile_options(n64_core PRIVATE -Wall -Wextra)
    # COP1 runs on the host FPU between fesetround / fetestexcept: keep the compiler from moving or folding it.
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        set_source_files_properties(n64_fpu.c PROPERTIES COMPILE_OPTIONS "-frounding-math;-fno-fast-math")
    endif()
    if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(n64_core PRIVATE -O2) # the interpreter is far too slow unoptimised
    endif()
    set_target_properties(n64_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# n64_pack: an N64 title pack, the runtime's side (pack.json checks, the ROM's identity, dlopen of the table, the
# discovered code written at exit; n64_pack.h, XBA-174). No SDL: the tests link it alone.
if(NOT TARGET n64_pack)
    add_library(n64_pack STATIC n64_pack.c)
    target_include_directories(n64_pack PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(n64_pack PUBLIC n64_core ${CMAKE_DL_LIBS})
    target_compile_options(n64_pack PRIVATE -Wall -Wextra)
    set_target_properties(n64_pack PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# n64_core_exported(target): the executable exports the core's symbols, which a recompiled title pack (n64_recomp.h,
# n64_recomp_rt.h: n64_rc_*) binds to when it is dlopen'ed. The whole archive is linked so every symbol is there.
function(n64_core_exported tgt)
    set_target_properties(${tgt} PROPERTIES ENABLE_EXPORTS ON)
    if(APPLE)
        target_link_options(${tgt} PRIVATE "LINKER:-force_load,$<TARGET_FILE:n64_core>")
    else()
        target_link_libraries(${tgt} PRIVATE -Wl,--whole-archive n64_core -Wl,--no-whole-archive)
    endif()
    target_link_libraries(${tgt} PRIVATE ${CMAKE_DL_LIBS})
endfunction()

# N64_CORE_ONLY=ON skips n64_runtime (no xbr_host / SDL2), so a host without SDL2 can still build and test the core
# and the devices (like PS1_CORE_ONLY).
option(N64_CORE_ONLY "Build only n64_core, the RCP devices and n64_run, not n64_runtime (no xbr_host / SDL2)" OFF)
if(NOT N64_CORE_ONLY AND NOT TARGET xbr_host)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../src/host ${CMAKE_CURRENT_BINARY_DIR}/xbr_host)
endif()
# n64_runtime: the core + the host layer (n64_runtime_run plays a ROM: window, pads, audio, saves, pacing; XBA-174).
if(NOT N64_CORE_ONLY AND NOT TARGET n64_runtime)
    add_library(n64_runtime STATIC n64_runtime.c)
    target_include_directories(n64_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(n64_runtime PUBLIC n64_core n64_pack xbr_host)
    target_compile_options(n64_runtime PRIVATE -Wall -Wextra)
    set_target_properties(n64_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# n64_play: plays a ROM on a desktop through n64_runtime (XBA-174). The title key comes from the ROM as the app reads
# it (recomp_pack_rom_identify): src/platform/recomp_pack.c is compiled in (libc + recomp_cfg, which xbr_host brings),
# so a project that adds this directory alone needs nothing more. It loads title packs (--pack, tools/prepare/n64.py):
# the whole core is exported.
if(NOT ANDROID AND TARGET n64_runtime AND NOT TARGET n64_play)
    get_filename_component(N64_PLATFORM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/platform ABSOLUTE)
    add_executable(n64_play tools/n64_play.c ${N64_PLATFORM_DIR}/recomp_pack.c)
    n64_core_exported(n64_play) # first, so the whole archive comes before the plain one
    target_include_directories(n64_play PRIVATE ${N64_PLATFORM_DIR})
    target_link_libraries(n64_play PRIVATE n64_runtime ${CMAKE_DL_LIBS})
    target_compile_options(n64_play PRIVATE -Wall -Wextra)
endif()

# n64_run: the headless runner (desktop hosts only); --pack loads a recompiled title (static recompilation, XBA-169..171).
if(NOT ANDROID AND NOT TARGET n64_run)
    add_executable(n64_run tools/n64_run.c)
    n64_core_exported(n64_run) # first, so the whole archive comes before the plain one
    target_link_libraries(n64_run PRIVATE n64_core)
    # --rewind (XBA-194), --cheats (XBA-197): the ring and the cheat engine alone (libc), not the whole host layer.
    target_sources(n64_run PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/platform/recomp_rewind.c
                                   ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/platform/recomp_cheats.c)
    target_include_directories(n64_run PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/platform)
    target_compile_options(n64_run PRIVATE -Wall -Wextra)
endif()
