# PCSX2's GS (Direct3D 11 hardware renderer) as a static library for the port.
# Source: thirdparty/pcsx2 (PCSX2 v2.8.2, GPL-3.0+). Only the GS, the parts of
# common/ it uses and the bundled libraries they need are compiled; the
# emulator services the GS calls (Host, VMManager, ImGui, savestates...) are
# provided by src/pcsx2_host.cpp. See relatorios/pcsx2-gs-integracao.md.
set(PCSX2_ROOT "${WARRIORS_ROOT}/thirdparty/pcsx2")

if(NOT TARGET pcsx2_gs)
    set(PCSX2_3RD "${PCSX2_ROOT}/3rdparty")
    # The bundled libraries are declared here rather than through their own
    # CMakeLists: this runs as a deferred call, where add_subdirectory and
    # project() are not allowed.
    add_library(pcsx2_fmt STATIC "${PCSX2_3RD}/fmt/src/format.cc" "${PCSX2_3RD}/fmt/src/os.cc")
    target_include_directories(pcsx2_fmt PUBLIC "${PCSX2_3RD}/fmt/include")
    target_compile_options(pcsx2_fmt PRIVATE /utf-8)
    target_compile_options(pcsx2_fmt PUBLIC /utf-8)
    set(cpu "${PCSX2_3RD}/cpuinfo")
    add_library(pcsx2_cpuinfo STATIC
        ${cpu}/src/api.c ${cpu}/src/cache.c ${cpu}/src/init.c ${cpu}/src/log.c
        ${cpu}/src/x86/cache/descriptor.c ${cpu}/src/x86/cache/deterministic.c ${cpu}/src/x86/cache/init.c
        ${cpu}/src/x86/info.c ${cpu}/src/x86/init.c ${cpu}/src/x86/isa.c ${cpu}/src/x86/name.c
        ${cpu}/src/x86/topology.c ${cpu}/src/x86/uarch.c ${cpu}/src/x86/vendor.c ${cpu}/src/x86/windows/init.c)
    target_include_directories(pcsx2_cpuinfo PUBLIC "${cpu}/include" PRIVATE "${cpu}/src")
    target_compile_definitions(pcsx2_cpuinfo PRIVATE _WIN32_WINNT=0x0601 _CRT_SECURE_NO_WARNINGS
        CPUINFO_LOG_TO_STDIO=1 CPUINFO_LOG_LEVEL=2)
    # ImGui without its FreeType font backend (the port draws no on-screen UI).
    add_library(pcsx2_imgui STATIC
        "${PCSX2_3RD}/imgui/src/imgui.cpp"
        "${PCSX2_3RD}/imgui/src/imgui_draw.cpp"
        "${PCSX2_3RD}/imgui/src/imgui_tables.cpp"
        "${PCSX2_3RD}/imgui/src/imgui_widgets.cpp")
    target_include_directories(pcsx2_imgui PUBLIC "${PCSX2_3RD}/imgui/include")
    # stb_truetype as the font loader (PCSX2 uses FreeType).
    target_compile_definitions(pcsx2_imgui PUBLIC IMGUI_ENABLE_STB_TRUETYPE)

    set(common "${PCSX2_ROOT}/common")
    set(gs "${PCSX2_ROOT}/pcsx2/GS")
    add_library(pcsx2_gs STATIC
        # common/ (no image codecs, YAML, HTTP or x86 emitter)
        ${common}/AlignedMalloc.cpp ${common}/Assertions.cpp ${common}/Console.cpp ${common}/CrashHandler.cpp
        ${common}/DynamicLibrary.cpp ${common}/Error.cpp ${common}/FileSystem.cpp ${common}/HostSys.cpp
        ${common}/MemoryInterface.cpp ${common}/MemorySettingsInterface.cpp ${common}/MD5Digest.cpp ${common}/Perf.cpp
        ${common}/ProgressCallback.cpp ${common}/ReadbackSpinManager.cpp ${common}/Semaphore.cpp
        ${common}/SettingsWrapper.cpp ${common}/SmallString.cpp ${common}/StringUtil.cpp ${common}/TextureDecompress.cpp
        ${common}/Timer.cpp ${common}/WindowInfo.cpp ${common}/StackWalker.cpp
        ${common}/Windows/WinThreads.cpp ${common}/Windows/WinHostSys.cpp ${common}/Windows/WinMisc.cpp
        # GS core and the shared renderer code
        ${PCSX2_ROOT}/pcsx2/Pcsx2Config.cpp ${PCSX2_ROOT}/pcsx2/SourceLog.cpp ${WARRIORS_ROOT}/src/pcsx2_host.cpp
        ${WARRIORS_ROOT}/src/pcsx2_gs_bridge.cpp
        ${gs}/GS.cpp ${gs}/GSClut.cpp ${gs}/GSDrawingContext.cpp ${gs}/GSDrawingEnvironment.cpp
        ${gs}/GSLocalMemory.cpp ${gs}/GSPerfMon.cpp ${gs}/GSRingHeap.cpp ${gs}/GSState.cpp ${gs}/GSTables.cpp
        ${gs}/GSUtil.cpp ${gs}/GSVector.cpp ${gs}/MultiISA.cpp
        ${gs}/GSBlock.cpp ${gs}/GSLocalMemoryMultiISA.cpp ${gs}/GSXXH.cpp
        ${gs}/Renderers/Common/GSDevice.cpp ${gs}/Renderers/Common/GSDirtyRect.cpp
        ${gs}/Renderers/Common/GSFunctionMap.cpp ${gs}/Renderers/Common/GSRenderer.cpp
        ${gs}/Renderers/Common/GSTexture.cpp ${gs}/Renderers/Common/GSVertexTrace.cpp
        ${gs}/Renderers/Common/GSVertexTraceFMM.cpp
        ${gs}/Renderers/Null/GSRendererNull.cpp
        ${gs}/Renderers/HW/GSHwHack.cpp ${gs}/Renderers/HW/GSRendererHW.cpp ${gs}/Renderers/HW/GSRendererHWMultiISA.cpp
        ${gs}/Renderers/HW/GSTextureCache.cpp
        ${gs}/Renderers/SW/GSTextureCacheSW.cpp ${gs}/Renderers/SW/GSDrawScanline.cpp ${gs}/Renderers/SW/GSRasterizer.cpp
        ${gs}/Renderers/SW/GSRendererSW.cpp ${gs}/Renderers/SW/GSDrawScanlineCodeGenerator.all.cpp
        ${gs}/Renderers/SW/GSSetupPrimCodeGenerator.all.cpp
        # Direct3D 11
        ${gs}/Renderers/DX11/D3D.cpp ${gs}/Renderers/DX11/D3D11ShaderCache.cpp
        ${gs}/Renderers/DX11/GSDevice11.cpp ${gs}/Renderers/DX11/GSTexture11.cpp)
    # PCSX2's build settings stay private: the game includes only the plain
    # interface in src/pcsx2_host.h (UNICODE and the like would change it).
    set(PCSX2_GS_INCLUDES
        "${PCSX2_ROOT}" "${PCSX2_ROOT}/pcsx2" "${PCSX2_3RD}/include" "${PCSX2_3RD}/xbyak"
        "${PCSX2_3RD}/d3d12memalloc/include" "${PCSX2_3RD}/ffmpeg/include"
        "${PCSX2_3RD}/fast_float/include" "${PCSX2_3RD}/winwil/include" "${WARRIORS_ROOT}/src"
        CACHE INTERNAL "")
    set(PCSX2_GS_DEFINITIONS
        _M_X86=1 __SSE4_1__=1 WINVER=0x0A00 _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN WIL_SUPPRESS_EXCEPTIONS
        _CRT_NONSTDC_NO_WARNINGS _CRT_SECURE_NO_WARNINGS CRT_SECURE_NO_DEPRECATE _SCL_SECURE_NO_WARNINGS
        _UNICODE UNICODE OVERRIDE_HOST_PAGE_SIZE=0x1000 OVERRIDE_HOST_CACHE_LINE_SIZE=64 PCSX2_GS_NO_DX12
        CACHE INTERNAL "")
    target_include_directories(pcsx2_gs PRIVATE ${PCSX2_GS_INCLUDES})
    target_compile_features(pcsx2_gs PRIVATE cxx_std_20)
    target_compile_definitions(pcsx2_gs PRIVATE ${PCSX2_GS_DEFINITIONS})
    # AVX2 throughout instead of PCSX2's per-ISA copies of the GS sources.
    target_compile_options(pcsx2_gs PRIVATE /Z7 /arch:AVX2 /fp:contract /GS- /Zc:__cplusplus /Zc:preprocessor /utf-8 /W0 /MP)
    target_link_libraries(pcsx2_gs PRIVATE pcsx2_fmt pcsx2_cpuinfo pcsx2_imgui
        d3d11 d3d12 dxgi d3dcompiler dxguid winmm pathcch onecore)
endif()

if(NOT TARGET pcsx2_gs_probe)
    add_executable(pcsx2_gs_probe "${WARRIORS_ROOT}/tools/pcsx2_gs_probe.cpp")
    target_include_directories(pcsx2_gs_probe PRIVATE ${PCSX2_GS_INCLUDES})
    target_compile_definitions(pcsx2_gs_probe PRIVATE ${PCSX2_GS_DEFINITIONS})
    target_compile_features(pcsx2_gs_probe PRIVATE cxx_std_20)
    target_link_libraries(pcsx2_gs_probe PRIVATE pcsx2_gs pcsx2_fmt dbghelp)
    target_link_options(pcsx2_gs_probe PRIVATE /DEBUG)
    target_compile_options(pcsx2_gs_probe PRIVATE /Zc:__cplusplus /Zc:preprocessor /utf-8 /arch:AVX2)
endif()

if(NOT TARGET pcsx2_gs_replay)
    add_executable(pcsx2_gs_replay "${WARRIORS_ROOT}/tools/pcsx2_gs_replay.cpp")
    target_include_directories(pcsx2_gs_replay PRIVATE ${PCSX2_GS_INCLUDES})
    target_compile_definitions(pcsx2_gs_replay PRIVATE ${PCSX2_GS_DEFINITIONS})
    target_compile_features(pcsx2_gs_replay PRIVATE cxx_std_20)
    target_link_libraries(pcsx2_gs_replay PRIVATE pcsx2_gs pcsx2_fmt dbghelp)
    target_compile_options(pcsx2_gs_replay PRIVATE /Zc:__cplusplus /Zc:preprocessor /utf-8 /arch:AVX2)
endif()

# The game: PCSX2's GS is selected at run time with OROCHI_RENDERER=pcsx2.
if(TARGET ps2EntryRunner)
    target_sources(ps2EntryRunner PRIVATE "${WARRIORS_ROOT}/src/pcsx2_runtime.cpp" "${WARRIORS_ROOT}/src/pcsx2_window.cpp")
    set_source_files_properties("${WARRIORS_ROOT}/src/pcsx2_runtime.cpp" "${WARRIORS_ROOT}/src/pcsx2_window.cpp"
        TARGET_DIRECTORY ps2EntryRunner PROPERTIES SKIP_UNITY_BUILD_INCLUSION TRUE)
    target_link_libraries(ps2EntryRunner PRIVATE pcsx2_gs)
    target_compile_definitions(ps2EntryRunner PRIVATE OROCHI_PCSX2_GS=1)
    # The GS's shaders (dx11, and common for the CAS sharpening headers), next
    # to the executable. (A target of its own: POST_BUILD steps must live in
    # the game's directory.)
    if(NOT TARGET pcsx2_gs_resources)
        add_custom_target(pcsx2_gs_resources
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PCSX2_ROOT}/bin/resources/shaders/dx11"
                "$<TARGET_FILE_DIR:ps2EntryRunner>/pcsx2-resources/shaders/dx11"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PCSX2_ROOT}/bin/resources/shaders/common"
                "$<TARGET_FILE_DIR:ps2EntryRunner>/pcsx2-resources/shaders/common")
    endif()
    add_dependencies(ps2EntryRunner pcsx2_gs_resources)
endif()
