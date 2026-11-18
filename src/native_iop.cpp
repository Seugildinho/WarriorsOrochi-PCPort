#include "native_iop.h"
#include "native_sif.h"
#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace {
uint16_t word(const uint8_t *p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
uint32_t dword(const uint8_t *p) {
    return uint32_t(word(p)) | (uint32_t(word(p + 2)) << 16);
}

bool copyIopElf(PS2Runtime &runtime, uint32_t address, std::vector<uint8_t> &image) {
    std::array<uint8_t, 52> header{};
    if (!runtime.readIopMemory(address, header.data(), header.size()) ||
        std::memcmp(header.data(), "\x7f" "ELF\x01\x01\x01", 7) != 0 || word(header.data() + 18) != 8) return false;
    const uint64_t ph = dword(header.data() + 28), sh = dword(header.data() + 32);
    const uint32_t phsize = word(header.data() + 42), phcount = word(header.data() + 44);
    const uint32_t shsize = word(header.data() + 46), shcount = word(header.data() + 48);
    if ((phcount && phsize < 32) || (shcount && shsize < 40)) return false;
    uint64_t size = std::max<uint64_t>({52, ph + uint64_t(phsize) * phcount, sh + uint64_t(shsize) * shcount});
    constexpr uint64_t limit = 2 * 1024 * 1024;
    if (size > limit) return false;
    image.resize(size_t(size));
    if (!runtime.readIopMemory(address, image.data(), image.size())) return false;
    for (uint32_t i = 0; i < phcount; ++i) {
        const auto *p = image.data() + ph + uint64_t(i) * phsize;
        size = std::max(size, uint64_t(dword(p + 4)) + dword(p + 16));
    }
    for (uint32_t i = 0; i < shcount; ++i) {
        const auto *p = image.data() + sh + uint64_t(i) * shsize;
        if (dword(p + 4) != 8) size = std::max(size, uint64_t(dword(p + 16)) + dword(p + 20));
    }
    if (size > limit) return false;
    image.resize(size_t(size));
    return runtime.readIopMemory(address, image.data(), image.size());
}

// The underscored SDK function receives an IOP address: the caller already
// transferred its IRX using SIF DMA. Upstream's public buffer helper reads EE
// addresses instead. Stage the real IOP bytes for the existing IRX loader.
void loadIopBuffer(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime) {
    const auto source = getRegU32(ctx, 4), size = getRegU32(ctx, 5);
    const auto arguments = getRegU32(ctx, 6), resultAddress = getRegU32(ctx, 7);
    const auto returnPc = getRegU32(ctx, 31);
    std::vector<uint8_t> image, args;
    bool valid = runtime && size <= 64 * 1024 && copyIopElf(*runtime, source, image);
    if (valid && size) {
        args.resize(size);
        for (uint32_t i = 0; i < size; ++i) {
            const auto *p = getConstMemPtr(ram, arguments + i);
            if (!p) { valid = false; break; }
            args[i] = *p;
        }
    }
    int32_t module = -1, startResult = -1;
    if (valid) {
        ensureOrochiSifCommandHandlers(*runtime);
        const auto directory = PS2Runtime::getIoPaths().hostRoot / "iop-staging";
        std::filesystem::create_directories(directory);
        const auto name = "module-" + std::to_string(source) + ".irx";
        std::ofstream output(directory / name, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char *>(image.data()), std::streamsize(image.size()));
        const bool saved = bool(output);
        output.close();
        if (saved) {
            const auto loaded = runtime->loadIopModule("host:iop-staging/" + name,
                args.empty() ? nullptr : args.data(), uint32_t(args.size()));
            module = loaded.handled ? loaded.moduleId : -1;
            startResult = loaded.startResult;
        }
    }
    std::cout << "[Orochi:iop-load] source=0x" << std::hex << source << std::dec
              << " imageBytes=" << image.size() << " module=" << module
              << " startResult=" << startResult << '\n';
    if (auto *out = getMemPtr(ram, resultAddress); resultAddress && out &&
        getMemPtr(ram, resultAddress + 3) == out + 3) std::memcpy(out, &startResult, 4);
    setReturnS32(ctx, module);
    ctx->pc = returnPc;
    if (module <= 0 && runtime) {
        std::cerr << "[Orochi:blocker] IOP module loading failed; stopping instead of retrying indefinitely.\n";
        runtime->requestStop();
    }
}
}

bool installOrochiIopFunctions(PS2Runtime &runtime) {
    return runtime.replaceFunction(0x0013f628, loadIopBuffer);
}
