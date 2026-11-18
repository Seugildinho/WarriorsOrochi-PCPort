// Offline check and timing of translated VU1 programs on recorded inputs.
// The game records them with OROCHI_VU1_SAMPLE=10,28 (src/native_vu1.cpp):
// each sample is the interpreter object as the translation received it plus
// the VU data memory. Every sample runs once on the interpreter and once
// translated (results compared like OROCHI_VU1_VERIFY), then the translation
// is timed on fresh copies.
// usage: orochi_vu1_bench <local/vu1_samples> <local/vu1> [repeats] [program...]
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_vu1.h"
#include "vu1_translation_access.h"
#include "vu1_translated.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace
{
struct Sample
{
    uint32_t program = 0, maxCycles = 0, entry = 0;
    uint64_t hash = 0;
    std::vector<uint8_t> object, data;
};

bool sameState(const VU1State &a, const VU1State &b)
{
    return std::memcmp(a.vf, b.vf, sizeof(a.vf)) == 0 && std::memcmp(a.vi, b.vi, sizeof(a.vi)) == 0 &&
        std::memcmp(a.acc, b.acc, sizeof(a.acc)) == 0 && std::memcmp(&a.q, &b.q, 4) == 0 &&
        std::memcmp(&a.p, &b.p, 4) == 0 && std::memcmp(&a.i, &b.i, 4) == 0 && a.r == b.r && a.pc == b.pc &&
        a.mac == b.mac && a.clip == b.clip && a.status == b.status && a.cycles == b.cycles &&
        a.ebit == b.ebit && a.branchPending == b.branchPending && a.top == b.top && a.itop == b.itop;
}

std::vector<Sample> load(const std::filesystem::path &path)
{
    std::vector<Sample> samples;
    std::ifstream in(path, std::ios::binary);
    for (;;)
    {
        uint32_t header[6];
        if (!in.read(reinterpret_cast<char *>(header), sizeof(header)))
            break;
        Sample s;
        if (header[0] != 0x31535556u || header[4] != sizeof(VU1Interpreter))
        {
            std::fprintf(stderr, "%s: bad sample header (object %u, expected %zu)\n", path.string().c_str(),
                         header[4], sizeof(VU1Interpreter));
            break;
        }
        s.program = header[1];
        s.maxCycles = header[2];
        s.entry = header[5];
        in.read(reinterpret_cast<char *>(&s.hash), sizeof(s.hash));
        s.object.resize(header[4]);
        s.data.resize(header[3]);
        in.read(reinterpret_cast<char *>(s.object.data()), s.object.size());
        if (!in.read(reinterpret_cast<char *>(s.data.data()), s.data.size()))
            break;
        samples.push_back(std::move(s));
    }
    return samples;
}

// The objects are allocated once: construction costs more than the programs.
void restore(VU1Interpreter &vu, const Sample &s)
{
    std::memcpy(static_cast<void *>(&vu), s.object.data(), sizeof(VU1Interpreter));
}
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: orochi_vu1_bench <samples dir> <code image dir> [repeats] [program...]\n");
        return 2;
    }
    const std::filesystem::path sampleDir = argv[1], codeDir = argv[2];
    const int repeats = argc > 3 ? std::max(1, std::atoi(argv[3])) : 20;
    std::vector<unsigned> only;
    for (int i = 4; i < argc; ++i)
        only.push_back(static_cast<unsigned>(std::atoi(argv[i])));
    auto gs = std::make_unique<GS>();
    auto expected = std::make_unique<VU1Interpreter>(), actual = std::make_unique<VU1Interpreter>();
    double totalNs = 0.0;
    unsigned totalMismatches = 0;
    for (unsigned index = 0; index < vu1TranslatedProgramCount; ++index)
    {
        if (!only.empty() && std::find(only.begin(), only.end(), index) == only.end())
            continue;
        const auto path = sampleDir / ("program-" + std::to_string(index) + ".bin");
        if (!std::filesystem::exists(path))
            continue;
        const auto samples = load(path);
        if (samples.empty())
            continue;
        const auto &entry = vu1TranslatedPrograms[index];
        char name[64];
        std::snprintf(name, sizeof(name), "vu1code-%016llx.bin", static_cast<unsigned long long>(entry.codeHash));
        std::ifstream codeFile(codeDir / name, std::ios::binary);
        std::vector<uint8_t> code((std::istreambuf_iterator<char>(codeFile)), std::istreambuf_iterator<char>());
        if (code.empty())
        {
            std::fprintf(stderr, "program %u: code image %s missing\n", index, name);
            continue;
        }
        unsigned mismatches = 0;
        uint64_t vuCycles = 0;
        double nanoseconds = 0.0;
        for (const Sample &s : samples)
        {
            restore(*expected, s);
            restore(*actual, s);
            std::vector<uint8_t> expectedData = s.data, actualData = s.data;
            VU1TranslationAccess::interpret(*expected, code.data(), static_cast<uint32_t>(code.size()),
                                            expectedData.data(), static_cast<uint32_t>(expectedData.size()), *gs,
                                            nullptr, s.maxCycles);
            VU1TranslationAccess::s_suppressPath1 = true;
            entry.program(*actual, actualData.data(), static_cast<uint32_t>(actualData.size()), *gs, nullptr,
                          s.maxCycles);
            if (!sameState(expected->state(), actual->state()) || expectedData != actualData ||
                VU1TranslationAccess::cycle(*expected) != VU1TranslationAccess::cycle(*actual))
            {
                if (++mismatches <= 3)
                    std::printf("program %u MISMATCH pc=0x%x/0x%x cycles=%llu/%llu mac=%x/%x status=%x/%x clip=%x/%x data=%d\n",
                                index, expected->state().pc, actual->state().pc,
                                static_cast<unsigned long long>(expected->state().cycles),
                                static_cast<unsigned long long>(actual->state().cycles), expected->state().mac,
                                actual->state().mac, expected->state().status, actual->state().status,
                                expected->state().clip, actual->state().clip, expectedData == actualData);
            }
            restore(*actual, s);
            const uint64_t firstCycle = VU1TranslationAccess::cycle(*actual);
            for (int r = 0; r < repeats; ++r)
            {
                restore(*actual, s);
                std::memcpy(actualData.data(), s.data.data(), s.data.size());
                const auto start = std::chrono::steady_clock::now();
                entry.program(*actual, actualData.data(), static_cast<uint32_t>(actualData.size()), *gs, nullptr,
                              s.maxCycles);
                nanoseconds += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
                vuCycles += VU1TranslationAccess::cycle(*actual) - firstCycle;
            }
            VU1TranslationAccess::s_suppressPath1 = false;
        }
        const double calls = static_cast<double>(samples.size()) * repeats;
        std::printf("program %u entry 0x%x samples=%zu mismatches=%u nsPerCall=%.0f vuCyclesPerCall=%.0f nsPerVuCycle=%.2f\n",
                    index, entry.entry, samples.size(), mismatches, nanoseconds / calls, vuCycles / calls,
                    nanoseconds / static_cast<double>(vuCycles));
        totalNs += nanoseconds / calls;
        totalMismatches += mismatches;
    }
    std::printf("total mismatches=%u sumNsPerCall=%.0f\n", totalMismatches, totalNs);
    return totalMismatches ? 1 : 0;
}
