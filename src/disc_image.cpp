// Finds the executable on a disc image (src/disc_image.h).
#include "disc_image.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <fstream>

namespace
{
    constexpr uint64_t kSectorSize = 2048;
    constexpr char kElfName[] = "SLUS_216.62;1";
    // The ELF's only code segment loads at this address, this far into the file.
    constexpr uint32_t kLoadAddress = 0x100000, kLoadOffset = 0x800;
    // English text of the executable: strings and their tables, no compiled
    // function, which is why a translation can change it.
    constexpr uint32_t kTextStart = 0x320000, kTextEnd = 0x340000;
    // FNV-1a of the rest of the USA executable, the one the code was compiled from.
    constexpr size_t kElfSize = 2462208;
    constexpr uint64_t kCodeFingerprint = 0x06b1ae0562ec1d5full;

    uint32_t littleEndian32(const char *bytes)
    {
        uint32_t value = 0;
        std::memcpy(&value, bytes, sizeof(value));
        return value;
    }

    bool readAt(std::ifstream &file, uint64_t offset, char *bytes, size_t count)
    {
        file.seekg(static_cast<std::streamoff>(offset));
        return file.read(bytes, static_cast<std::streamsize>(count)).good();
    }

    // The ISO9660 root directory entry of the executable, read into `out`.
    bool readDiscElf(const std::filesystem::path &disc, std::vector<char> &out)
    {
        std::ifstream file(disc, std::ios::binary);
        char descriptor[kSectorSize];
        if (!file || !readAt(file, 16 * kSectorSize, descriptor, sizeof(descriptor)))
            return false;
        if (std::memcmp(descriptor + 1, "CD001", 5) != 0)
            return false;
        const uint32_t rootSector = littleEndian32(descriptor + 156 + 2);
        const uint32_t rootSize = littleEndian32(descriptor + 156 + 10);
        std::vector<char> root(rootSize);
        if (rootSize == 0 || !readAt(file, rootSector * kSectorSize, root.data(), root.size()))
            return false;
        const size_t nameSize = sizeof(kElfName) - 1;
        for (size_t at = 0; at < root.size();)
        {
            const uint8_t size = static_cast<uint8_t>(root[at]);
            if (size == 0)
            {
                at = (at / kSectorSize + 1) * kSectorSize;
                continue;
            }
            if (at + size > root.size())
                return false;
            if (static_cast<uint8_t>(root[at + 32]) == nameSize && std::memcmp(&root[at + 33], kElfName, nameSize) == 0)
            {
                out.resize(littleEndian32(&root[at + 10]));
                return readAt(file, littleEndian32(&root[at + 2]) * kSectorSize, out.data(), out.size());
            }
            at += size;
        }
        return false;
    }

    bool matchesCompiledCode(const std::vector<char> &elf)
    {
        if (elf.size() != kElfSize)
            return false;
        const size_t textStart = kTextStart - kLoadAddress + kLoadOffset, textEnd = kTextEnd - kLoadAddress + kLoadOffset;
        uint64_t hash = 0xcbf29ce484222325ull;
        for (size_t at = 0; at < elf.size(); ++at)
            if (at < textStart || at >= textEnd)
                hash = (hash ^ static_cast<uint8_t>(elf[at])) * 0x100000001b3ull;
        return hash == kCodeFingerprint;
    }
}

bool orochiIsCompatibleDisc(const std::filesystem::path &disc)
{
    std::vector<char> elf;
    return readDiscElf(disc, elf) && matchesCompiledCode(elf);
}

std::vector<std::filesystem::path> orochiCompatibleDiscs(const std::filesystem::path &folder)
{
    std::vector<std::filesystem::path> discs;
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator(folder, error))
    {
        std::wstring extension = entry.path().extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
        if (extension == L".iso" && orochiIsCompatibleDisc(entry.path()))
            discs.push_back(entry.path());
    }
    std::sort(discs.begin(), discs.end());
    return discs;
}

bool orochiExtractDiscElf(const std::filesystem::path &disc, const std::filesystem::path &extractTo, std::string &error)
{
    std::vector<char> elf;
    if (!readDiscElf(disc, elf))
    {
        error = "The disc image has no SLUS_216.62 executable.";
        return false;
    }
    if (!matchesCompiledCode(elf))
    {
        error = "This disc's code differs from the compiled game; only its text may change.";
        return false;
    }
    std::ofstream file(extractTo, std::ios::binary | std::ios::trunc);
    file.write(elf.data(), static_cast<std::streamsize>(elf.size()));
    if (!file.flush())
    {
        error = "The disc's executable could not be written next to the port.";
        return false;
    }
    return true;
}
