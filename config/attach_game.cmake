# Included after PS2Recomp's first project() call. Keep upstream as CMake root,
# since its dependency modules use CMAKE_SOURCE_DIR-relative paths.
function(warriors_attach_generated_code)
    if(NOT TARGET ps2EntryRunner)
        return()
    endif()
    get_filename_component(WARRIORS_ROOT "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    set(WARRIORS_GENERATED "${WARRIORS_ROOT}/generated")
    file(GLOB WARRIORS_SOURCES CONFIGURE_DEPENDS "${WARRIORS_GENERATED}/*.cpp")
    if(NOT EXISTS "${WARRIORS_GENERATED}/ps2_recompiled_functions.h" OR NOT WARRIORS_SOURCES)
        message(FATAL_ERROR "Generate Warriors Orochi code before configuring its native runner")
    endif()
    # The upstream empty function table is replaced by this game's generated table.
    set_source_files_properties(
        "${CMAKE_SOURCE_DIR}/ps2xRuntime/src/runner/register_functions.cpp"
        "${CMAKE_SOURCE_DIR}/ps2xRuntime/src/main.cpp"
        TARGET_DIRECTORY ps2EntryRunner PROPERTIES HEADER_FILE_ONLY TRUE)
    target_sources(ps2EntryRunner PRIVATE ${WARRIORS_SOURCES}
        "${WARRIORS_ROOT}/src/main.cpp" "${WARRIORS_ROOT}/src/native_cd.cpp"
        "${WARRIORS_ROOT}/src/native_iop.cpp" "${WARRIORS_ROOT}/src/diagnostics.cpp"
        "${WARRIORS_ROOT}/src/dma_regression.cpp" "${WARRIORS_ROOT}/src/graphics_trace.cpp"
        "${WARRIORS_ROOT}/src/native_image.cpp" "${WARRIORS_ROOT}/src/native_pad.cpp"
        "${WARRIORS_ROOT}/src/native_overlay.cpp" "${WARRIORS_ROOT}/src/switch_regression.cpp"
        "${WARRIORS_ROOT}/src/native_sif.cpp" "${WARRIORS_ROOT}/src/iop_memory_regression.cpp"
        "${WARRIORS_ROOT}/src/iop_loader_regression.cpp" "${WARRIORS_ROOT}/src/syncv_regression.cpp"
        "${WARRIORS_ROOT}/src/mpeg_audio_regression.cpp" "${WARRIORS_ROOT}/src/native_mpeg.cpp"
        "${WARRIORS_ROOT}/src/native_gpu.cpp" "${WARRIORS_ROOT}/src/native_ipu_decoder.cpp"
        "${WARRIORS_ROOT}/src/native_vu1.cpp" "${WARRIORS_ROOT}/src/gs_thread_backend.cpp"
        "${WARRIORS_ROOT}/src/game_patches.cpp" "${WARRIORS_ROOT}/src/game_settings.cpp"
        "${WARRIORS_ROOT}/src/orochi_settings.cpp" "${WARRIORS_ROOT}/src/setup_win32.cpp"
        "${WARRIORS_ROOT}/src/disc_image.cpp" "${WARRIORS_ROOT}/src/auto_save.cpp"
        "${WARRIORS_ROOT}/src/cheats.cpp" "${WARRIORS_ROOT}/src/draw_distance.cpp"
        "${WARRIORS_ROOT}/src/orochi.rc")
    set_source_files_properties("${WARRIORS_ROOT}/src/game_settings.cpp" "${WARRIORS_ROOT}/src/orochi_settings.cpp"
        "${WARRIORS_ROOT}/src/setup_win32.cpp" "${WARRIORS_ROOT}/src/disc_image.cpp"
        TARGET_DIRECTORY ps2EntryRunner PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    # The second program: chooses the disc and options the game starts with.
    if(WIN32 AND NOT TARGET orochi_setup)
        add_executable(orochi_setup WIN32 "${WARRIORS_ROOT}/src/setup_screen.cpp"
            "${WARRIORS_ROOT}/src/orochi_settings.cpp" "${WARRIORS_ROOT}/src/setup_win32.cpp"
            "${WARRIORS_ROOT}/src/disc_image.cpp" "${WARRIORS_ROOT}/src/orochi.rc")
        target_compile_features(orochi_setup PRIVATE cxx_std_20)
        target_link_libraries(orochi_setup PRIVATE raylib comdlg32)
        target_compile_definitions(orochi_setup PRIVATE "$<$<BOOL:${OROCHI_PCSX2_GS}>:OROCHI_PCSX2_GS=1>")
        # Its Portuguese strings are UTF-8, as raylib expects.
        target_compile_options(orochi_setup PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
        target_link_options(orochi_setup PRIVATE /ENTRY:mainCRTStartup)
        set_target_properties(orochi_setup PROPERTIES OUTPUT_NAME "WarriorsOrochi-Setup"
            RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:ps2EntryRunner>")
        add_dependencies(ps2EntryRunner orochi_setup)
        target_link_libraries(ps2EntryRunner PRIVATE comdlg32)
    endif()
    # Unity batches are for generated code only: hand-written sources keep
    # private helpers with common names, and batch contents shift whenever
    # generated files are added.
    file(GLOB WARRIORS_HAND_SOURCES "${WARRIORS_ROOT}/src/*.cpp")
    set_source_files_properties(${WARRIORS_HAND_SOURCES} TARGET_DIRECTORY ps2EntryRunner
        PROPERTIES SKIP_UNITY_BUILD_INCLUSION TRUE)
    # Ahead-of-time VU1 translations: the generator runs the interpreter's own
    # decode (orochi_vu1_dump) over the private code images in local/vu1.
    if(NOT TARGET orochi_vu1_dump)
        add_executable(orochi_vu1_dump "${WARRIORS_ROOT}/tools/vu1_usage_dump.cpp")
        target_link_libraries(orochi_vu1_dump PRIVATE ps2_runtime)
        set_target_properties(orochi_vu1_dump PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/vu1")
    endif()
    find_program(WARRIORS_PYTHON NAMES python python3 REQUIRED)
    set(WARRIORS_VU1_OUT "${CMAKE_BINARY_DIR}/vu1/generated")
    file(GLOB WARRIORS_VU1_IMAGES CONFIGURE_DEPENDS "${WARRIORS_ROOT}/local/vu1/vu1code-*.bin"
        "${WARRIORS_ROOT}/local/vu1/vu0code-*.bin")
    add_custom_command(
        OUTPUT "${WARRIORS_VU1_OUT}/vu1_translated.cpp" "${WARRIORS_VU1_OUT}/vu1_translated.h"
        COMMAND "${WARRIORS_PYTHON}" -I "${WARRIORS_ROOT}/tools/generate_vu1.py"
                "$<TARGET_FILE:orochi_vu1_dump>" "${WARRIORS_VU1_OUT}"
        DEPENDS orochi_vu1_dump "${WARRIORS_ROOT}/tools/generate_vu1.py"
                "${WARRIORS_ROOT}/config/vu1_programs.json" ${WARRIORS_VU1_IMAGES}
        COMMENT "Translating VU1 microprograms"
        VERBATIM)
    # The runner lives in another CMake directory, which cannot see this
    # directory's custom command outputs; order it through a custom target.
    add_custom_target(orochi_vu1_translations DEPENDS
        "${WARRIORS_VU1_OUT}/vu1_translated.cpp" "${WARRIORS_VU1_OUT}/vu1_translated.h")
    add_dependencies(ps2EntryRunner orochi_vu1_translations)
    target_sources(ps2EntryRunner PRIVATE "${WARRIORS_VU1_OUT}/vu1_translated.cpp")
    set_source_files_properties("${WARRIORS_VU1_OUT}/vu1_translated.cpp" "${WARRIORS_VU1_OUT}/vu1_translated.h"
        TARGET_DIRECTORY ps2EntryRunner PROPERTIES GENERATED TRUE SKIP_UNITY_BUILD_INCLUSION TRUE
        # The shared PCH has no debug info; using it drops this file's line info (LNK4206).
        SKIP_PRECOMPILE_HEADERS TRUE)
    if(MSVC)
        # Translated programs call small VU1 helpers on every instruction; let
        # link-time code generation inline them from the runtime library.
        # /Z7: line info for the sampling profiler (does not change codegen).
        # /d2OptimizeHugeFunctions (and /d2:-OptimizeHugeFunctions for the
        # link-time code generation): MSVC silently compiles functions above
        # its size limit without optimization (C4883); the clipping programs
        # (~24k generated lines) ran as debug-level code.
        set_property(SOURCE "${WARRIORS_VU1_OUT}/vu1_translated.cpp" TARGET_DIRECTORY ps2EntryRunner
            APPEND PROPERTY COMPILE_OPTIONS "$<$<NOT:$<CONFIG:Debug>>:/Ob3>" "$<$<NOT:$<CONFIG:Debug>>:/GL>" /Z7
            /d2OptimizeHugeFunctions)
        target_link_options(ps2EntryRunner PRIVATE "$<$<NOT:$<CONFIG:Debug>>:/d2:-OptimizeHugeFunctions>")
    endif()
    target_include_directories(ps2EntryRunner PRIVATE "${WARRIORS_VU1_OUT}" "${WARRIORS_ROOT}/src")
    # Offline check/timing of the translations on inputs recorded with
    # OROCHI_VU1_SAMPLE (tools/vu1_bench_main.cpp). Same compile options.
    if(NOT TARGET orochi_vu1_bench)
        add_executable(orochi_vu1_bench "${WARRIORS_ROOT}/tools/vu1_bench_main.cpp"
            "${WARRIORS_VU1_OUT}/vu1_translated.cpp")
        add_dependencies(orochi_vu1_bench orochi_vu1_translations)
        target_link_libraries(orochi_vu1_bench PRIVATE ps2_runtime)
        target_include_directories(orochi_vu1_bench PRIVATE "${WARRIORS_VU1_OUT}" "${WARRIORS_ROOT}/src")
        set_target_properties(orochi_vu1_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/vu1")
        if(MSVC)
            target_compile_options(orochi_vu1_bench PRIVATE "$<$<NOT:$<CONFIG:Debug>>:/Ob3>"
                "$<$<NOT:$<CONFIG:Debug>>:/GL>" /Z7 /d2OptimizeHugeFunctions)
            target_link_options(orochi_vu1_bench PRIVATE "$<$<NOT:$<CONFIG:Debug>>:/LTCG>" /DEBUG
                "$<$<NOT:$<CONFIG:Debug>>:/d2:-OptimizeHugeFunctions>")
        endif()
    endif()
    if(WIN32)
        # D3D headers must not inherit raylib's Windows exclusion macros from
        # unrelated sources when generated code changes the unity grouping.
        set_source_files_properties("${WARRIORS_ROOT}/src/native_gpu.cpp"
            TARGET_DIRECTORY ps2EntryRunner PROPERTIES SKIP_UNITY_BUILD_INCLUSION TRUE)
        target_link_libraries(ps2EntryRunner PRIVATE d3d11 d3dcompiler dxgi)
        add_executable(orochi_gpu_check "${WARRIORS_ROOT}/tools/gpu_check_main.cpp" "${WARRIORS_ROOT}/src/native_gpu.cpp")
        target_link_libraries(orochi_gpu_check PRIVATE ps2_runtime d3d11 d3dcompiler dxgi)
        target_include_directories(orochi_gpu_check PRIVATE "${WARRIORS_ROOT}/src")
    endif()
    # 2026-10-23: in-process IPU (ipu2) decoder check against the FFmpeg CLI reference.
    # Built beside the runner so it finds the same staged FFmpeg DLLs.
    if(NOT TARGET orochi_ipu_check)
        add_executable(orochi_ipu_check "${WARRIORS_ROOT}/tools/ipu_check_main.cpp" "${WARRIORS_ROOT}/src/native_ipu_decoder.cpp")
        target_link_libraries(orochi_ipu_check PRIVATE ffmpeg)
        target_include_directories(orochi_ipu_check PRIVATE "${WARRIORS_ROOT}/src")
        set_target_properties(orochi_ipu_check PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:ps2EntryRunner>")
    endif()
    target_include_directories(ps2EntryRunner BEFORE PRIVATE "${WARRIORS_GENERATED}")
    set_target_properties(ps2EntryRunner PROPERTIES OUTPUT_NAME "WarriorsOrochi")
    # Optimise release builds without changing the translated floating-point rules.
    # Debug retains the original unoptimised diagnostic build.
    if(MSVC)
        target_compile_options(ps2EntryRunner PRIVATE
            "$<$<CONFIG:Debug>:/Od>" "$<$<NOT:$<CONFIG:Debug>>:/O2>"
            /fp:strict /bigobj /MP4)
        # Pixel sampling and writes cross these translation units millions of
        # times per frame. Let MSVC inline them across the runtime library.
        # The VU1 interpreter's upper/lower units call small core helpers on
        # every instruction; battle profiles spend ~75% of frame time there.
        set_property(SOURCE
            "${CMAKE_SOURCE_DIR}/ps2xRuntime/src/lib/gs/gs_cpu_backend.cpp"
            "${CMAKE_SOURCE_DIR}/ps2xRuntime/src/lib/gs/ps2_gs_memory.cpp"
            "${CMAKE_SOURCE_DIR}/ps2xRuntime/src/lib/vu/ps2_vu1_core.cpp"
            "${CMAKE_SOURCE_DIR}/ps2xRuntime/src/lib/vu/ps2_vu1_lower.cpp"
            "${CMAKE_SOURCE_DIR}/ps2xRuntime/src/lib/vu/ps2_vu1_upper.cpp"
            TARGET_DIRECTORY ps2_runtime APPEND PROPERTY COMPILE_OPTIONS
            "$<$<NOT:$<CONFIG:Debug>>:/Ob3>" "$<$<NOT:$<CONFIG:Debug>>:/GL>")
        # The IOP interpreter runs RPC handlers synchronously on the game
        # thread; title and menu screens are bound by its per-instruction
        # calls into memory, import and timer code.
        if(TARGET ps2_iop)
            set_property(SOURCE
                "${CMAKE_SOURCE_DIR}/ps2xIOP/src/emulator/iop_emulator.cpp"
                "${CMAKE_SOURCE_DIR}/ps2xIOP/src/emulator/core/iop_cpu.cpp"
                "${CMAKE_SOURCE_DIR}/ps2xIOP/src/emulator/core/iop_memory.cpp"
                "${CMAKE_SOURCE_DIR}/ps2xIOP/src/emulator/core/iop_kernel.cpp"
                "${CMAKE_SOURCE_DIR}/ps2xIOP/src/emulator/imports/iop_imports.cpp"
                "${CMAKE_SOURCE_DIR}/ps2xIOP/src/emulator/imports/iop_timrman.cpp"
                TARGET_DIRECTORY ps2_iop APPEND PROPERTY COMPILE_OPTIONS
                "$<$<NOT:$<CONFIG:Debug>>:/Ob3>" "$<$<NOT:$<CONFIG:Debug>>:/GL>")
        endif()
        target_link_options(ps2EntryRunner PRIVATE "$<$<NOT:$<CONFIG:Debug>>:/LTCG>")
        target_link_options(ps2EntryRunner PRIVATE /DEBUG:FULL)
        # A game, not a console tool: launchers and scripts still redirect its output.
        target_link_options(ps2EntryRunner PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
        # Full symbols for the runtime and IOP libraries (codegen unchanged):
        # without them the sampling profiler attributes static functions and
        # lambdas to whatever public symbol precedes them.
        target_compile_options(ps2_runtime PRIVATE "$<$<NOT:$<CONFIG:Debug>>:/Z7>")
        target_compile_options(ps2EntryRunner PRIVATE "$<$<NOT:$<CONFIG:Debug>>:/Z7>")
        if(TARGET ps2_iop)
            target_compile_options(ps2_iop PRIVATE "$<$<NOT:$<CONFIG:Debug>>:/Z7>")
        endif()
    endif()
    if(WIN32 AND NOT TARGET orochi_stack_probe)
        add_executable(orochi_stack_probe "${WARRIORS_ROOT}/tools/stack_probe.cpp")
        target_compile_definitions(orochi_stack_probe PRIVATE _WIN32_WINNT=0x0A00)
        target_link_libraries(orochi_stack_probe PRIVATE dbghelp)
        set_target_properties(orochi_stack_probe PROPERTIES OUTPUT_NAME "OrochiStackProbe"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/diagnostics")
    endif()
    list(LENGTH WARRIORS_SOURCES WARRIORS_SOURCE_COUNT)
    message(STATUS "Warriors Orochi: ${WARRIORS_SOURCE_COUNT} generated compilation units attached")
endfunction()
cmake_language(DEFER CALL warriors_attach_generated_code)

# PCSX2's GS as an alternative renderer (relatorios/pcsx2-gs-integracao.md).
# Deferred: this file runs inside PS2Recomp's project(), where the bundled
# libraries' own project() calls are not allowed.
option(OROCHI_PCSX2_GS "Build PCSX2's GS (Direct3D 11 hardware renderer)" OFF)
function(warriors_attach_pcsx2_gs)
    if(OROCHI_PCSX2_GS)
        get_filename_component(WARRIORS_ROOT "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
        include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/pcsx2_gs.cmake")
    endif()
endfunction()
cmake_language(DEFER CALL warriors_attach_pcsx2_gs)
