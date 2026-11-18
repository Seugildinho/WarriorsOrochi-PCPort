#include "native_cd.h"
#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
constexpr uint32_t sectorSize = 2048;
uint32_t le32(const uint8_t *bytes) {
    return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) |
           (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
}
struct DiscFile { uint32_t sector{}, size{}; std::string name; };

std::string normalize(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    if (auto colon = path.find(':'); colon != std::string::npos) path.erase(0, colon + 1);
    while (!path.empty() && path.front() == '/') path.erase(0, 1);
    if (auto version = path.find(';'); version != std::string::npos) path.erase(version);
    std::transform(path.begin(), path.end(), path.begin(),
                   [](unsigned char ch) { return char(std::toupper(ch)); });
    return path;
}

bool readAt(std::ifstream &iso, uint64_t offset, uint8_t *out, size_t length) {
    iso.clear();
    iso.seekg(static_cast<std::streamoff>(offset));
    return bool(iso.read(reinterpret_cast<char *>(out), static_cast<std::streamsize>(length)));
}

// Read actual ISO9660 directory records. Sector numbers remain the original
// disc's numbers, so the runtime's sector reader uses the same ISO directly.
bool findOnDisc(const std::string &request, DiscFile &file) {
    std::ifstream iso(PS2Runtime::getIoPaths().cdImage, std::ios::binary);
    std::array<uint8_t, sectorSize> descriptor{};
    if (!iso || !readAt(iso, 16ull * sectorSize, descriptor.data(), descriptor.size()) ||
        descriptor[0] != 1 || std::memcmp(descriptor.data() + 1, "CD001", 5) != 0 ||
        descriptor[128] != 0 || descriptor[129] != 8) return false;
    uint32_t directorySector = le32(descriptor.data() + 158);
    uint32_t directorySize = le32(descriptor.data() + 166);
    const auto path = normalize(request);
    size_t componentStart = 0;
    while (componentStart < path.size()) {
        const auto slash = path.find('/', componentStart);
        const bool last = slash == std::string::npos;
        const auto component = path.substr(componentStart, last ? std::string::npos : slash - componentStart);
        if (component.empty() || component == "." || component == ".." ||
            directorySize == 0 || directorySize > 16 * 1024 * 1024) return false;
        std::vector<uint8_t> records(directorySize);
        if (!readAt(iso, uint64_t(directorySector) * sectorSize, records.data(), records.size())) return false;
        bool found = false;
        for (size_t at = 0; at < records.size();) {
            const size_t length = records[at];
            if (!length) { at = ((at / sectorSize) + 1) * sectorSize; continue; }
            if (length < 34 || at + length > records.size()) return false;
            const auto *record = records.data() + at;
            const size_t nameLength = record[32];
            if (33 + nameLength > length) return false;
            const std::string name(reinterpret_cast<const char *>(record + 33), nameLength);
            if (nameLength && uint8_t(name[0]) > 1 && normalize(name) == component) {
                directorySector = le32(record + 2);
                directorySize = le32(record + 10);
                const bool isDirectory = (record[25] & 2) != 0;
                if (last) {
                    if (isDirectory) return false;
                    file = {directorySector, directorySize, normalize(name)};
                    return true;
                }
                if (!isDirectory) return false;
                found = true;
                break;
            }
            at += length;
        }
        if (!found || last) return false;
        componentStart = slash + 1;
    }
    return false;
}

void search(uint8_t *ram, R5900Context *ctx, PS2Runtime *, bool layered) {
    const auto resultAddress = getRegU32(ctx, 4);
    const auto nameAddress = getRegU32(ctx, 5);
    const auto layer = layered ? getRegU32(ctx, 6) : 0;
    std::string name;
    bool terminated = false;
    for (uint32_t i = 0; i < 260; ++i) {
        const auto *byte = getConstMemPtr(ram, nameAddress + i);
        if (!byte) break;
        if (!*byte) { terminated = true; break; }
        name.push_back(char(*byte));
    }
    auto *out = getMemPtr(ram, resultAddress);
    DiscFile file;
    // This USA disc is single-layer; unsupported layers fail explicitly.
    const bool ok = layer == 0 && terminated && out &&
                    getMemPtr(ram, resultAddress + 31) == out + 31 && findOnDisc(name, file);
    if (ok) {
        // sceCdlFILE: u32 sector, u32 size, char name[16], u8 date[8].
        std::array<uint8_t, 32> packed{};
        std::memcpy(packed.data(), &file.sector, 4);
        std::memcpy(packed.data() + 4, &file.size, 4);
        std::memcpy(packed.data() + 8, file.name.data(), std::min<size_t>(15, file.name.size()));
        std::memcpy(out, packed.data(), packed.size());
    }
    std::cout << "[Orochi:disc-search] " << name << " layer=" << layer
              << " found=" << ok << " sector=" << file.sector << " bytes=" << file.size << '\n';
    setReturnS32(ctx, ok ? 1 : 0);
    ctx->pc = getRegU32(ctx, 31);
}
void searchFile(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime) { search(ram, ctx, runtime, false); }
void layerSearchFile(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime) { search(ram, ctx, runtime, true); }
}

bool installOrochiDiscFunctions(PS2Runtime &runtime) {
    // Addresses identified by PS2Recomp signatures and confirmed in native
    // trace: the original layer-search wrapper binds CD RPC SID 0x80000597.
    return runtime.replaceFunction(0x001281b0, layerSearchFile) &&
           runtime.replaceFunction(0x001284b0, searchFile);
}
