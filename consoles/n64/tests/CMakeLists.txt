cmake_minimum_required(VERSION 3.20)
project(console_n64_tests C)

# macOS: _POSIX_C_SOURCE hides the BSD / C99 extras (snprintf, mkdtemp...) unless _DARWIN_C_SOURCE is set.
if(APPLE)
    add_compile_definitions(_DARWIN_C_SOURCE)
endif()
# The Nintendo 64 runtime's C tests (XBA-146): a self-contained project like those under tests/, run by
# tools/run_c_tests.sh (as console_n64) and by the root build's ctest when XBR_CONSOLE_N64 is ON. POSIX hosts.
#
#   n64_runtime       the runtime: helpers, headless runs of gfx.z64 (saves, fd:N, .v64, the app shell's entry);
#                     needs xbr_host / SDL2, not built with N64_CORE_ONLY=ON
#   n64_rdp           RDP feature tests on synthetic command lists (test_rdp.c, with n64_rdp.c); n64_rdp_bench, its
#                     benchmark, runs by hand (n64_rdp_bench [seconds per case])
#   n64_rsp           RSP vectors and differential groups vs ares (test_rsp.c); n64_rsp_rdp: RSP microcode driving
#                     the real RDP over XBUS (test_rsp_rdp.c); n64_rsp_bench runs by hand
#   n64_cpu           core unit tests (test_cpu.c): integer unit, delay slots, exceptions, interrupts, TLB, COP1
#   n64_io            core unit tests (test_io.c): ROM byte orders / header, PIF boot state, joybus (paks, EEPROM
#                     type, RTC), PI, SRAM, FlashRAM, the AI and its resampler (tones, rate change), the VI picture
#                     path (canvas, scaling, filters, gamma, interlace)
#   n64_hb_<prog>     our homebrew ROMs under n64_run --result (HLE PIF, libdragon IPL3 run as on the hardware);
#                     rsp needs the real RSP (n64_rsp.c), gfx the real RSP and RDP (n64_rsp.c, n64_rdp.c)
#   n64_rc_*          the same ROMs recompiled (n64_tools.recomp + cc, discovery loop) in lockstep with the
#                     interpreter; n64_rc_systemtest the suite under --pack; n64_rc_fuzz random MIPS III code
#   n64_state_*       save states (XBA-183, state_rom.sh): round trips on the homebrew ROMs (interpreter, packs as
#                     n64_rc_state_*, the Vulkan RDP), another ROM's state refused
#   n64_rewind_*      rewind round trips on the homebrew ROMs (n64_run --rewind, XBA-194)
#   n64_cheats_*      GameShark codes on the homebrew (n64_run --cheats, XBA-197)
#   n64_systemtest    lemmy-64/n64-systemtest (MIT) built from source: every CPU / COP0 / TLB / COP1 / memory test
#                     must pass but the known gaps listed in systemtest_known.txt (RSP / RDP tests only count with
#                     the real modules); with the real RSP / RDP at least N64_SYSTEMTEST_PASS test values must pass
#                     (XBA-202); n64_systemtest_full: the ROM with every test group (timing, cycle, cop0hazard,
#                     quirks), at least N64_SYSTEMTEST_FULL_PASS
#   n64_libdragon_tests libdragon's test ROM (Unlicense, XBA-202): every test passes but libdragon_known.txt, at least
#                     N64_LIBDRAGON_PASS of them; on the Vulkan RDP too (_vulkan), and recompiled in lockstep (n64_rc_)
#   n64_rsp_inval_instrs sauraen/rsp_inval_instrs (Unlicense, XBA-202): the RSP's invalid SPECIAL opcodes as measured on
#                     hardware ("Test passed")
# The ROM tests skip (exit 77) when their inputs are absent, so a CI job without Docker or network stays green.
# They come from (repo root):
#   tools/consoles/build_homebrew.sh n64 build/homebrew     -> N64_HOMEBREW_DIR (default build/homebrew/n64)
#   tools/consoles/build_n64_systemtest.sh build/n64-systemtest -> N64_SYSTEMTEST (default
#                                                              build/n64-systemtest/n64-systemtest.z64; and
#                                                              n64-systemtest-full.z64, N64_SYSTEMTEST_FULL)
#   tools/consoles/build_n64_libdragon_tests.sh build/n64-libdragon-tests -> N64_LIBDRAGON_TESTROM (default
#                                                              build/n64-libdragon-tests/testrom_emu.z64), and
#                                                              rsp_inval_instrs.z64 next to it
if(WIN32)
    message(FATAL_ERROR "The console runtimes are POSIX-only (xbr_host)")
endif()
set(CMAKE_C_STANDARD 11)
if(XBR_N64_RDP_VULKAN)
    enable_language(CXX) # the Vulkan RDP (paraLLEl-RDP) is C++: the executables here link it
endif()

if(NOT TARGET n64_core)
    add_subdirectory(../runtime n64_runtime)
endif()

enable_testing()

get_filename_component(N64_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
set(N64_HOMEBREW_DIR ${N64_REPO_ROOT}/build/homebrew/n64 CACHE PATH "Built N64 homebrew (build_homebrew.sh n64)")

# The runtime (XBA-174): helpers always, headless runs of gfx.z64 when the homebrew is built.
if(TARGET n64_runtime)
    add_executable(n64_runtime_test test_main.c)
    target_link_libraries(n64_runtime_test PRIVATE n64_runtime)
    target_compile_options(n64_runtime_test PRIVATE -Wall -Wextra)
    add_test(NAME n64_runtime COMMAND n64_runtime_test ${N64_HOMEBREW_DIR})
    set_tests_properties(n64_runtime PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
endif()

if(TARGET n64_rdp AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/n64_rdp.c)
    add_executable(n64_rdp_test test_rdp.c)
    target_link_libraries(n64_rdp_test PRIVATE n64_rdp m)
    target_compile_options(n64_rdp_test PRIVATE -Wall -Wextra)
    add_test(NAME n64_rdp COMMAND n64_rdp_test)
    set_tests_properties(n64_rdp PROPERTIES TIMEOUT 120)
    # The Vulkan RDP (XBR_N64_RDP_VULKAN, XBA-173): the same tests on it, and both RDPs on the same lists
    # (n64_rdp_test --compare, a report); skipped (77) without a Vulkan device. Without a GPU, Mesa's lavapipe
    # (mesa-vulkan-drivers; VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json) runs them, slowly: a device and its
    # pipelines are made per test case.
    if(TARGET n64_rdp_vk)
        add_test(NAME n64_rdp_vulkan COMMAND n64_rdp_test)
        set_tests_properties(n64_rdp_vulkan PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800
                             ENVIRONMENT "RECOMP_N64_RDP=vulkan")
        add_test(NAME n64_rdp_compare COMMAND n64_rdp_test --compare)
        set_tests_properties(n64_rdp_compare PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800)
        # A custom Vulkan driver that cannot be opened (n64_vk_driver.h; off Android libxba_vkdriver.so is absent,
        # the path of a failed open on a device): the log says why, the system driver runs the same tests.
        add_test(NAME n64_rdp_vulkan_bad_driver COMMAND n64_rdp_test)
        set_tests_properties(n64_rdp_vulkan_bad_driver PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800
                             ENVIRONMENT "RECOMP_N64_RDP=vulkan;RECOMP_VK_DRIVER_DIR=${CMAKE_CURRENT_BINARY_DIR}/vk_drivers/none/;RECOMP_VK_DRIVER_LIB=vulkan.none.so")
    endif()
    add_executable(n64_rdp_bench bench_rdp.c)
    target_link_libraries(n64_rdp_bench PRIVATE n64_rdp m)
    target_compile_options(n64_rdp_bench PRIVATE -Wall -Wextra -O2)
endif()

# The RSP (N-5, XBA-173): hand-checked vectors and the differential groups of homebrew/common/rsp_oracle.h against
# the checksums recorded from ares (rsp_oracle_expected.h), with a fake RDP (test_rsp.c); test_rsp_rdp.c drives the
# real RDP through the DPC registers (an XBUS command list built by RSP microcode). n64_rsp_bench, RSP instructions/s
# on a vector-heavy microcode loop, runs by hand (n64_rsp_bench [seconds]).
if(TARGET n64_rsp AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/n64_rsp.c)
    add_executable(n64_rsp_test test_rsp.c)
    target_link_libraries(n64_rsp_test PRIVATE n64_rsp)
    target_compile_options(n64_rsp_test PRIVATE -Wall -Wextra)
    add_test(NAME n64_rsp COMMAND n64_rsp_test)
    set_tests_properties(n64_rsp PROPERTIES TIMEOUT 120)
    add_executable(n64_rsp_bench bench_rsp.c)
    target_link_libraries(n64_rsp_bench PRIVATE n64_rsp)
    target_compile_options(n64_rsp_bench PRIVATE -Wall -Wextra -O2)
    if(TARGET n64_rdp AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/n64_rdp.c)
        add_executable(n64_rsp_rdp_test test_rsp_rdp.c)
        target_link_libraries(n64_rsp_rdp_test PRIVATE n64_rsp n64_rdp m)
        target_compile_options(n64_rsp_rdp_test PRIVATE -Wall -Wextra -Wno-unused-function) # rdp_cmd.h scenes
        add_test(NAME n64_rsp_rdp COMMAND n64_rsp_rdp_test)
        set_tests_properties(n64_rsp_rdp PROPERTIES TIMEOUT 60)
    endif()
endif()

# The custom Vulkan driver selection of the N64 RDP (n64_vk_driver.h): RECOMP_VK_*, the trial marker, last_start. Plain
# C, no Vulkan: built in every configuration.
add_executable(n64_vk_driver_test test_vk_driver.c ../runtime/n64_vk_driver.c)
target_include_directories(n64_vk_driver_test PRIVATE ../runtime)
target_link_libraries(n64_vk_driver_test PRIVATE ${CMAKE_DL_LIBS})
target_compile_options(n64_vk_driver_test PRIVATE -Wall -Wextra)
add_test(NAME n64_vk_driver COMMAND n64_vk_driver_test)
set_tests_properties(n64_vk_driver PROPERTIES TIMEOUT 30)
# A fake libxba_vkdriver.so that opens any driver (fake_vkdriver.c): the outcomes after an open (ok, unsupported,
# not_loaded) in last_start and <id>.status. Not on Windows (no dlopen).
if(NOT WIN32)
    add_library(n64_fake_vkdriver MODULE fake_vkdriver.c)
    set_target_properties(n64_fake_vkdriver PROPERTIES PREFIX "" OUTPUT_NAME libxba_vkdriver SUFFIX ".so"
                          LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/fake_vkdriver)
    target_link_libraries(n64_fake_vkdriver PRIVATE ${CMAKE_DL_LIBS})
    add_dependencies(n64_vk_driver_test n64_fake_vkdriver)
    add_test(NAME n64_vk_driver_wrapper COMMAND n64_vk_driver_test --wrapper)
    set_tests_properties(n64_vk_driver_wrapper PROPERTIES TIMEOUT 30
        ENVIRONMENT "LD_LIBRARY_PATH=${CMAKE_CURRENT_BINARY_DIR}/fake_vkdriver;DYLD_LIBRARY_PATH=${CMAKE_CURRENT_BINARY_DIR}/fake_vkdriver")
endif()

foreach(t cpu io)
    add_executable(n64_${t}_test test_${t}.c)
    target_link_libraries(n64_${t}_test PRIVATE n64_core)
    target_compile_options(n64_${t}_test PRIVATE -Wall -Wextra)
    add_test(NAME n64_${t} COMMAND n64_${t}_test)
    set_tests_properties(n64_${t} PROPERTIES TIMEOUT 60)
endforeach()

set(N64_SYSTEMTEST ${N64_REPO_ROOT}/build/n64-systemtest/n64-systemtest.z64 CACHE FILEPATH
    "n64-systemtest ROM (build_n64_systemtest.sh)")
set(N64_SYSTEMTEST_FULL ${N64_REPO_ROOT}/build/n64-systemtest/n64-systemtest-full.z64 CACHE FILEPATH
    "n64-systemtest ROM with every test group (build_n64_systemtest.sh)")
set(N64_LIBDRAGON_TESTROM ${N64_REPO_ROOT}/build/n64-libdragon-tests/testrom_emu.z64 CACHE FILEPATH
    "libdragon's test ROM (build_n64_libdragon_tests.sh)")
# Pass-count thresholds of the open test ROMs (XBA-202), with the real RSP / RDP: a regression fails even in a test the
# known-gap lists cover. Raise them with the core; lower only for a test proven wrong.
set(N64_SYSTEMTEST_PASS 3716)       # of 3721 test values (base group): 5 need cached data
set(N64_SYSTEMTEST_FULL_PASS 3726)  # of 3731 (base, timing, cycle, cop0hazard, poorly understood quirks)
set(N64_LIBDRAGON_PASS 102)         # of 107 (4 skipped under emulation): test_cache_invalidate needs cached data
set(N64_RT ${CMAKE_CURRENT_SOURCE_DIR}/../runtime)
set(N64_HAVE_RCP OFF)
if(EXISTS ${N64_RT}/n64_rsp.c AND EXISTS ${N64_RT}/n64_rdp.c)
    set(N64_HAVE_RCP ON)
endif()

if(TARGET n64_run)
    set(hb hello cpu timer_irq)
    if(EXISTS ${N64_RT}/n64_rsp.c)
        list(APPEND hb rsp)
    endif()
    if(N64_HAVE_RCP)
        list(APPEND hb gfx)
    endif()
    foreach(prog ${hb})
        add_test(NAME n64_hb_${prog} COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_rom.sh $<TARGET_FILE:n64_run>
                 ${N64_HOMEBREW_DIR}/${prog}.z64 --result --frames 900 --quiet)
        set_tests_properties(n64_hb_${prog} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
    endforeach()
    # Internal resolution (XBA-185): gfx.z64 on the Vulkan RDP at 2x, a 1280 x 480 scanout matching the 1x picture.
    if(N64_HAVE_RCP AND TARGET n64_rdp_vk)
        add_test(NAME n64_hb_gfx_upscale COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_upscale.sh $<TARGET_FILE:n64_run>
                 ${N64_HOMEBREW_DIR}/gfx.z64 ${CMAKE_CURRENT_BINARY_DIR}/upscale)
        set_tests_properties(n64_hb_gfx_upscale PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 400)
    endif()
    # Rewind (XBA-194, n64_run --rewind): 600 frames with pseudo-random pads, a capture every 4 frames as the runtime
    # takes them, 45 steps back: the picture there and the frames after it must be the first run's.
    foreach(prog ${hb})
        add_test(NAME n64_rewind_hb_${prog} COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_rom.sh $<TARGET_FILE:n64_run>
                 ${N64_HOMEBREW_DIR}/${prog}.z64 --frames 600 --rewind 4:45 --quiet)
        set_tests_properties(n64_rewind_hb_${prog} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
    endforeach()
    # Cheats (XBA-197, n64_run --cheats cheats/hello.cheats): GameShark 80 / 81 writes, a D1 conditional on the result
    # block's status, a 50 serial repeater, an F1 boot write next to an enabler code, and a cheat that is off, read
    # back after the verdict (--peek; big-endian words).
    string(CONCAT want "PASS|peek 0x803001B0 = 0x12345600|peek 0x803001B4 = 0xBEEF0000|peek 0x803001B8 = 0x00100011"
           "|peek 0x803001BC = 0x00120000|peek 0x80380000 = 0x77770000")
    add_test(NAME n64_cheats_hb_hello COMMAND sh ${N64_REPO_ROOT}/tools/consoles/expect_output.sh "${want}"
             sh ${CMAKE_CURRENT_SOURCE_DIR}/run_rom.sh $<TARGET_FILE:n64_run> ${N64_HOMEBREW_DIR}/hello.z64 --result
             --frames 900 --quiet --cheats ${CMAKE_CURRENT_SOURCE_DIR}/cheats/hello.cheats
             --peek 0x803001B0 --peek 0x803001B4 --peek 0x803001B8 --peek 0x803001BC --peek 0x80380000)
    set_tests_properties(n64_cheats_hb_hello PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
    # Save states (XBA-183, state_rom.sh): the round trip (a state saved after frame 150, the next 150 frames' picture
    # and audio hashed, then the same from the state loaded in a new machine) on every homebrew ROM, gfx's also
    # loaded with 2 and 4 RDP threads; with each ROM's pack below (n64_rc_state_*: states cross between lifted and
    # interpreted runs); gfx on the Vulkan RDP (its round trip, states crossing back ends, upscaled 2x); refusals:
    # another ROM's state, a truncated one.
    set(N64_STATE sh ${CMAKE_CURRENT_SOURCE_DIR}/state_rom.sh $<TARGET_FILE:n64_run>)
    set(N64_STATE_WORK ${CMAKE_CURRENT_BINARY_DIR}/state)
    foreach(prog ${hb})
        add_test(NAME n64_state_hb_${prog} COMMAND ${N64_STATE} ${N64_HOMEBREW_DIR}/${prog}.z64
                 ${N64_STATE_WORK}/hb_${prog} interp)
        set_tests_properties(n64_state_hb_${prog} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
    endforeach()
    add_test(NAME n64_state_wrong_game COMMAND ${N64_STATE} ${N64_HOMEBREW_DIR}/timer_irq.z64 ${N64_STATE_WORK}/other
             other:${N64_HOMEBREW_DIR}/hello.z64)
    set_tests_properties(n64_state_wrong_game PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
    if(N64_HAVE_RCP AND TARGET n64_rdp_vk)
        add_test(NAME n64_state_vulkan_gfx COMMAND ${N64_STATE} ${N64_HOMEBREW_DIR}/gfx.z64 ${N64_STATE_WORK}/vulkan
                 vulkan)
        set_tests_properties(n64_state_vulkan_gfx PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800)
    endif()
    add_test(NAME n64_systemtest COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_systemtest.sh $<TARGET_FILE:n64_run>
             ${N64_SYSTEMTEST} ${CMAKE_CURRENT_SOURCE_DIR}/systemtest_known.txt ${N64_HAVE_RCP})
    set_tests_properties(n64_systemtest PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
    if(N64_HAVE_RCP)
        set_tests_properties(n64_systemtest PROPERTIES ENVIRONMENT "N64_SYSTEMTEST_MIN_PASS=${N64_SYSTEMTEST_PASS}")
        add_test(NAME n64_systemtest_full COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/run_systemtest.sh
                 $<TARGET_FILE:n64_run> ${N64_SYSTEMTEST_FULL} ${CMAKE_CURRENT_SOURCE_DIR}/systemtest_known.txt ON)
        set_tests_properties(n64_systemtest_full PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300
                             ENVIRONMENT "N64_SYSTEMTEST_MIN_PASS=${N64_SYSTEMTEST_FULL_PASS}")
        set(N64_LDT sh ${CMAKE_CURRENT_SOURCE_DIR}/run_libdragon_tests.sh $<TARGET_FILE:n64_run> ${N64_LIBDRAGON_TESTROM}
            ${CMAKE_CURRENT_SOURCE_DIR}/libdragon_known.txt)
        add_test(NAME n64_libdragon_tests COMMAND ${N64_LDT})
        set_tests_properties(n64_libdragon_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 700
                             ENVIRONMENT "N64_LIBDRAGON_MIN_PASS=${N64_LIBDRAGON_PASS}")
        get_filename_component(N64_LDT_DIR ${N64_LIBDRAGON_TESTROM} DIRECTORY)
        add_test(NAME n64_rsp_inval_instrs COMMAND sh ${N64_REPO_ROOT}/tools/consoles/expect_output.sh "Test passed after"
                 sh ${CMAKE_CURRENT_SOURCE_DIR}/run_rom.sh $<TARGET_FILE:n64_run> ${N64_LDT_DIR}/rsp_inval_instrs.z64
                 --tty --frames 3000 --quiet)
        set_tests_properties(n64_rsp_inval_instrs PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
        if(TARGET n64_rdp_vk)
            add_test(NAME n64_libdragon_tests_vulkan COMMAND ${N64_LDT})
            set_tests_properties(n64_libdragon_tests_vulkan PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1200
                                 ENVIRONMENT "N64_LIBDRAGON_MIN_PASS=${N64_LIBDRAGON_PASS};RECOMP_N64_RDP=vulkan")
        endif()
    endif()

    # Static recompilation (N-1 XBA-169, N-2 XBA-170, N-3 XBA-171): each homebrew ROM lifted to C from the ROM alone
    # (n64_tools.recomp; its ELF only measures the discovery: >= 99 % of the function symbols found), built with the
    # discovery loop and run in lockstep with the interpreter (recomp_rom.sh: the whole state compared after every
    # frame); n64-systemtest under --pack (the interpreter's pass / known-gap set) and in lockstep
    # (recomp_systemtest.sh); n64_rc_fuzz: random MIPS III code under COUNT = COMPARE interrupts (recomp_fuzz.py).
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        set(N64_RC_WORK ${CMAKE_CURRENT_BINARY_DIR}/recomp)
        foreach(prog ${hb})
            add_test(NAME n64_rc_hb_${prog} COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/recomp_rom.sh $<TARGET_FILE:n64_run>
                     ${N64_HOMEBREW_DIR}/${prog}.z64 ${N64_RC_WORK}/hb_${prog} 900 --result)
            set_tests_properties(n64_rc_hb_${prog} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800
                                 ENVIRONMENT "PYTHON=${Python3_EXECUTABLE}")
        endforeach()
        foreach(prog ${hb})
            add_test(NAME n64_rc_state_hb_${prog} COMMAND ${N64_STATE} ${N64_HOMEBREW_DIR}/${prog}.z64
                     ${N64_STATE_WORK}/rc_${prog} pack)
            set_tests_properties(n64_rc_state_hb_${prog} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 2400
                                 ENVIRONMENT "PYTHON=${Python3_EXECUTABLE}")
        endforeach()
        add_test(NAME n64_rc_systemtest COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/recomp_systemtest.sh
                 $<TARGET_FILE:n64_run> ${N64_SYSTEMTEST} ${CMAKE_CURRENT_SOURCE_DIR}/systemtest_known.txt
                 ${N64_HAVE_RCP} ${N64_RC_WORK}/systemtest)
        set_tests_properties(n64_rc_systemtest PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 3600
                             ENVIRONMENT "PYTHON=${Python3_EXECUTABLE}")
        if(N64_HAVE_RCP)
            set_tests_properties(n64_rc_systemtest PROPERTIES
                ENVIRONMENT "PYTHON=${Python3_EXECUTABLE};N64_SYSTEMTEST_MIN_PASS=${N64_SYSTEMTEST_PASS}")
            add_test(NAME n64_rc_libdragon_tests COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/recomp_libdragon_tests.sh
                     $<TARGET_FILE:n64_run> ${N64_LIBDRAGON_TESTROM} ${CMAKE_CURRENT_SOURCE_DIR}/libdragon_known.txt
                     ${N64_RC_WORK}/libdragon_tests)
            set_tests_properties(n64_rc_libdragon_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 3600
                ENVIRONMENT "PYTHON=${Python3_EXECUTABLE};N64_LIBDRAGON_MIN_PASS=${N64_LIBDRAGON_PASS}")
        endif()
        add_test(NAME n64_rc_fuzz COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/recomp_fuzz.py
                 --n64-run $<TARGET_FILE:n64_run> --work ${N64_RC_WORK}/fuzz --seeds 0-7 --frames 15 --insns 1500)
        set_tests_properties(n64_rc_fuzz PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800)
    endif()
endif()

# An N64 title pack (XBA-174): fake_n64_pack is built like tools/prepare's libtitle.so (its core calls resolved against
# the program that loads it), n64_pack_test exports the core and checks n64_pack.h (the homebrew gfx.z64's identity
# too when it is built).
add_library(fake_n64_pack SHARED fake_pack.c)
target_include_directories(fake_n64_pack PRIVATE ../runtime)
if(APPLE)
    target_link_options(fake_n64_pack PRIVATE -undefined dynamic_lookup)
endif()
add_executable(n64_pack_test test_pack.c)
n64_core_exported(n64_pack_test)
target_link_libraries(n64_pack_test PRIVATE n64_pack)
target_compile_options(n64_pack_test PRIVATE -Wall -Wextra)
add_test(NAME n64_pack COMMAND n64_pack_test $<TARGET_FILE:fake_n64_pack> ${N64_HOMEBREW_DIR})
set_tests_properties(n64_pack PROPERTIES TIMEOUT 60)
