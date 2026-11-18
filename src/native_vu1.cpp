#include "native_vu1.h"
#include "ps2_runtime.h"
#include "vu1_translation_access.h"
#include "vu1_translated.h"
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <memory>
#include <vector>

namespace {
// OROCHI_VU1_TRANSLATION=0 keeps every microprogram on the interpreter.
// OROCHI_VU1_CAPTURE=1 saves each new VU1 code image to local/vu1/ so
//   tools/generate_vu1.py can translate it (game code stays private).
// OROCHI_VU1_VERIFY=N runs the interpreter for the first N translated calls,
//   replays the translation on a copy and reports any state/memory difference.
bool captureImages{};
// VU0 programs run on the EE thread and VU1 programs on the VIF1 thread; the
// diagnostic modes below share counters and maps, so they are serialized.
std::mutex diagnosticsMutex;
// OROCHI_VU1_INVENTORY=1 reports entry points (VU1 and VU0) that still run on
//   the interpreter, with their call count and average/maximum cycles.
bool inventory{};
struct UntranslatedStat { uint64_t calls{}, cycles{}, maxCycles{}; uint32_t budget{}; };
std::map<std::pair<uint64_t, uint32_t>, UntranslatedStat> untranslated;
thread_local UntranslatedStat *measuredEntry{};
thread_local const uint8_t *measuredCode{};
thread_local uint32_t measuredCodeSize{};
uint64_t lookups{};
unsigned verifyRemaining{};
unsigned verifiedCalls{}, verifyFailures{};
thread_local VU1Interpreter::TranslatedProgram verifyTarget{};
// OROCHI_VU1_BENCH=1 times sampled translated calls: every 16th call is
//   replayed 8 times on copies (PATH1 suppressed) before the real run, and
//   ns per call for each program is printed at exit. Scene independent
//   enough to compare generator changes.
bool bench{};
struct BenchStat { uint64_t calls{}, nanoseconds{}, vuCycles{}; };
std::array<BenchStat, 64> benchStats{};
uint64_t benchCounter{};
thread_local unsigned benchIndex{};
thread_local const uint8_t *verifyCode{};
thread_local uint32_t verifyCodeSize{};
// OROCHI_VU1_SAMPLE=10,28 saves the inputs of every 61st call of those
//   programs (interpreter object + VU data, up to 2000 each) to
//   local/vu1_samples/ for tools/vu1_bench_main.cpp (orochi_vu1_bench).
bool sampling{};
std::array<bool, 64> sampledPrograms{};
std::array<uint32_t, 64> sampleCalls{};
std::array<uint32_t, 64> sampleSaved{};
thread_local unsigned sampleIndex{};
thread_local uint64_t sampleHash{};

void captureImage(uint64_t hash, const uint8_t *code, uint32_t size) {
    static std::vector<uint64_t> saved;
    for (const auto known : saved) if (known == hash) return;
    saved.push_back(hash);
    const auto directory = PS2Runtime::getIoPaths().elfDirectory / "vu1";
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    char name[64];
    std::snprintf(name, sizeof(name), "%s-%016llx.bin", size == 0x1000u ? "vu0code" : "vu1code",
                  static_cast<unsigned long long>(hash));
    const auto path = directory / name;
    if (std::filesystem::exists(path, ignored)) return;
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(code), size);
    std::cout << "[Orochi:vu1] captured code image " << name << '\n';
}

bool sameState(const VU1State &a, const VU1State &b) {
    return std::memcmp(a.vf, b.vf, sizeof(a.vf)) == 0 && std::memcmp(a.vi, b.vi, sizeof(a.vi)) == 0 &&
        std::memcmp(a.acc, b.acc, sizeof(a.acc)) == 0 && std::memcmp(&a.q, &b.q, 4) == 0 &&
        std::memcmp(&a.p, &b.p, 4) == 0 && std::memcmp(&a.i, &b.i, 4) == 0 && a.r == b.r && a.pc == b.pc &&
        a.mac == b.mac && a.clip == b.clip && a.status == b.status && a.cycles == b.cycles &&
        a.ebit == b.ebit && a.branchPending == b.branchPending && a.top == b.top && a.itop == b.itop;
}

void reportDifference(const VU1State &expected, const VU1State &actual, const uint8_t *expectedData,
                      const uint8_t *actualData, uint32_t dataSize) {
    std::cout << std::hex << "[Orochi:vu1-verify] MISMATCH pc=0x" << expected.pc << "/0x" << actual.pc
              << " cycles=" << std::dec << expected.cycles << "/" << actual.cycles << std::hex
              << " mac=0x" << expected.mac << "/0x" << actual.mac << " status=0x" << expected.status << "/0x"
              << actual.status << " clip=0x" << expected.clip << "/0x" << actual.clip << '\n';
    for (int reg = 0; reg < 32; ++reg)
        if (std::memcmp(expected.vf[reg], actual.vf[reg], 16) != 0) {
            uint32_t e[4], a[4];
            std::memcpy(e, expected.vf[reg], 16); std::memcpy(a, actual.vf[reg], 16);
            std::cout << "  vf" << std::dec << reg << std::hex << " exp=" << e[0] << ',' << e[1] << ',' << e[2] << ','
                      << e[3] << " got=" << a[0] << ',' << a[1] << ',' << a[2] << ',' << a[3] << '\n';
        }
    for (int reg = 0; reg < 16; ++reg)
        if (expected.vi[reg] != actual.vi[reg])
            std::cout << "  vi" << std::dec << reg << " exp=" << expected.vi[reg] << " got=" << actual.vi[reg] << '\n';
    if (std::memcmp(expected.acc, actual.acc, 16) != 0) std::cout << "  acc differs\n";
    if (std::memcmp(&expected.q, &actual.q, 4) != 0) std::cout << "  q differs\n";
    unsigned shown = 0;
    for (uint32_t offset = 0; offset < dataSize && shown < 8; offset += 16)
        if (std::memcmp(expectedData + offset, actualData + offset, 16) != 0) {
            std::cout << "  vuData qword 0x" << std::hex << (offset / 16) << " differs\n";
            ++shown;
        }
    std::cout << std::dec;
}

void verifyProgram(VU1Interpreter &vu, uint8_t *vuData, uint32_t dataSize, GS &gs, PS2Memory *memory,
                   uint32_t maxCycles) {
    std::lock_guard lock(diagnosticsMutex);
    const auto target = verifyTarget;
    auto copy = std::make_unique<VU1Interpreter>(vu);
    std::vector<uint8_t> dataCopy(vuData, vuData + dataSize);
    // The game keeps the interpreter's result; the translation runs on the copy.
    VU1TranslationAccess::interpret(vu, const_cast<uint8_t *>(verifyCode), verifyCodeSize, vuData, dataSize, gs,
                                    memory, maxCycles);
    VU1TranslationAccess::s_suppressPath1 = true;
    target(*copy, dataCopy.data(), dataSize, gs, memory, maxCycles);
    VU1TranslationAccess::s_suppressPath1 = false;
    ++verifiedCalls;
    const bool sameData = std::memcmp(vuData, dataCopy.data(), dataSize) == 0;
    const bool sameCycle = VU1TranslationAccess::cycle(vu) == VU1TranslationAccess::cycle(*copy);
    if (!sameState(vu.state(), copy->state()) || !sameData || !sameCycle) {
        if (++verifyFailures <= 4)
            reportDifference(vu.state(), copy->state(), vuData, dataCopy.data(), dataSize);
    }
    if (verifiedCalls % 500 == 0 || verifyRemaining == 1)
        std::cout << "[Orochi:vu1-verify] checked=" << verifiedCalls << " mismatches=" << verifyFailures << '\n';
}

void reportBench();

void benchProgram(VU1Interpreter &vu, uint8_t *vuData, uint32_t dataSize, GS &gs, PS2Memory *memory,
                  uint32_t maxCycles) {
    std::lock_guard lock(diagnosticsMutex);
    const unsigned index = benchIndex;
    const auto target = vu1TranslatedPrograms[index].program;
    if (++benchCounter % 16 == 0 && index < benchStats.size()) {
        std::vector<uint8_t> data(dataSize);
        VU1TranslationAccess::s_suppressPath1 = true;
        for (int repeat = 0; repeat < 8; ++repeat) {
            auto copy = std::make_unique<VU1Interpreter>(vu);
            std::memcpy(data.data(), vuData, dataSize);
            const auto start = std::chrono::steady_clock::now();
            const uint64_t firstCycle = VU1TranslationAccess::cycle(*copy);
            target(*copy, data.data(), dataSize, gs, memory, maxCycles);
            const auto elapsed = std::chrono::steady_clock::now() - start;
            benchStats[index].vuCycles += VU1TranslationAccess::cycle(*copy) - firstCycle;
            benchStats[index].nanoseconds +=
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
            ++benchStats[index].calls;
        }
        VU1TranslationAccess::s_suppressPath1 = false;
        if (benchCounter % (16u * 20000u) == 0) reportBench();
    }
    target(vu, vuData, dataSize, gs, memory, maxCycles);
}

void sampleProgram(VU1Interpreter &vu, uint8_t *vuData, uint32_t dataSize, GS &gs, PS2Memory *memory,
                   uint32_t maxCycles) {
    std::unique_lock lock(diagnosticsMutex);
    const unsigned index = sampleIndex;
    const auto target = vu1TranslatedPrograms[index].program;
    if (++sampleCalls[index] % 61u == 0u && sampleSaved[index] < 2000u) {
        ++sampleSaved[index];
        const auto directory = PS2Runtime::getIoPaths().elfDirectory / "vu1_samples";
        std::error_code ignored;
        std::filesystem::create_directories(directory, ignored);
        std::ofstream out(directory / ("program-" + std::to_string(index) + ".bin"), std::ios::binary | std::ios::app);
        const uint32_t header[6] = {0x31535556u, index, maxCycles, dataSize,
                                    static_cast<uint32_t>(sizeof(VU1Interpreter)), vu1TranslatedPrograms[index].entry};
        out.write(reinterpret_cast<const char *>(header), sizeof(header));
        out.write(reinterpret_cast<const char *>(&sampleHash), sizeof(sampleHash));
        out.write(reinterpret_cast<const char *>(&vu), sizeof(VU1Interpreter));
        out.write(reinterpret_cast<const char *>(vuData), dataSize);
    }
    lock.unlock();
    target(vu, vuData, dataSize, gs, memory, maxCycles);
}

void reportBench() {
    uint64_t calls = 0, nanoseconds = 0;
    for (unsigned index = 0; index < vu1TranslatedProgramCount && index < benchStats.size(); ++index) {
        const auto &stat = benchStats[index];
        if (!stat.calls) continue;
        calls += stat.calls; nanoseconds += stat.nanoseconds;
        std::cout << "[Orochi:vu1-bench] program=" << index << " entry=0x" << std::hex
                  << vu1TranslatedPrograms[index].entry << std::dec << " samples=" << stat.calls
                  << " nsPerCall=" << stat.nanoseconds / stat.calls
                  << " vuCyclesPerCall=" << stat.vuCycles / stat.calls << '\n';
    }
    if (calls)
        std::cout << "[Orochi:vu1-bench] total samples=" << calls << " nsPerCall=" << nanoseconds / calls << '\n';
}

void reportInventory() {
    std::cout << "[Orochi:vu1-inventory] lookups=" << lookups << " untranslated entries:";
    for (const auto &[key, stat] : untranslated)
        std::cout << ' ' << std::hex << key.first << '@' << key.second << std::dec << '=' << stat.calls
                  << "/avg" << (stat.calls ? stat.cycles / stat.calls : 0) << "/max" << stat.maxCycles
                  << "/budget" << stat.budget;
    std::cout << std::endl;
}

// Runs an untranslated program on the interpreter and records its cycles.
void measureProgram(VU1Interpreter &vu, uint8_t *vuData, uint32_t dataSize, GS &gs, PS2Memory *memory,
                    uint32_t maxCycles) {
    std::lock_guard lock(diagnosticsMutex);
    auto &stat = *measuredEntry;
    const uint64_t first = VU1TranslationAccess::cycle(vu);
    VU1TranslationAccess::interpret(vu, const_cast<uint8_t *>(measuredCode), measuredCodeSize, vuData, dataSize, gs,
                                    memory, maxCycles);
    const uint64_t cycles = VU1TranslationAccess::cycle(vu) - first;
    ++stat.calls; stat.cycles += cycles; stat.budget = maxCycles;
    if (cycles > stat.maxCycles) stat.maxCycles = cycles;
    static uint64_t measured = 0;
    if (++measured % 20000 == 0) reportInventory();
}

VU1Interpreter::TranslatedProgram lookupTranslation(uint64_t hash, uint32_t startPc, const uint8_t *code,
                                                   uint32_t size) {
    std::unique_lock<std::mutex> lock(diagnosticsMutex, std::defer_lock);
    if (captureImages || inventory || verifyRemaining || bench || sampling) lock.lock();
    if (captureImages) captureImage(hash, code, size);
    if (inventory && ++lookups % 50000 == 0) reportInventory();
    for (unsigned index = 0; index < vu1TranslatedProgramCount; ++index) {
        const auto &entry = vu1TranslatedPrograms[index];
        if (entry.program && entry.codeHash == hash && entry.entry == startPc) {
            if (verifyRemaining) {
                --verifyRemaining;
                verifyTarget = entry.program;
                verifyCode = code;
                verifyCodeSize = size;
                return verifyProgram;
            }
            if (bench) {
                benchIndex = index;
                return benchProgram;
            }
            if (index < sampledPrograms.size() && sampledPrograms[index]) {
                sampleIndex = index;
                sampleHash = hash;
                return sampleProgram;
            }
            return entry.program;
        }
    }
    if (inventory) {
        measuredEntry = &untranslated[{hash, startPc}];
        measuredCode = code;
        measuredCodeSize = size;
        return measureProgram;
    }
    return nullptr;
}
}

void installOrochiVu1Translations() {
    const char *disabled = std::getenv("OROCHI_VU1_TRANSLATION");
    captureImages = std::getenv("OROCHI_VU1_CAPTURE") != nullptr;
    inventory = std::getenv("OROCHI_VU1_INVENTORY") != nullptr;
    if (const char *verify = std::getenv("OROCHI_VU1_VERIFY")) verifyRemaining = static_cast<unsigned>(std::strtoul(verify, nullptr, 10));
    bench = std::getenv("OROCHI_VU1_BENCH") != nullptr;
    if (const char *sample = std::getenv("OROCHI_VU1_SAMPLE"))
        for (const char *p = sample; *p;) {
            char *next = nullptr;
            const unsigned long index = std::strtoul(p, &next, 10);
            if (next == p) break;
            if (index < sampledPrograms.size()) sampledPrograms[index] = sampling = true;
            p = *next == ',' ? next + 1 : next;
        }
    if (bench) std::atexit(reportBench);
    // Lookups are cached per code image, so the periodic report may never fire.
    if (inventory) std::atexit(reportInventory);
    if (disabled && std::string(disabled) == "0" && !captureImages) return;
    std::cout << "[Orochi:vu1] translated programs=" << vu1TranslatedProgramCount
              << " verify=" << verifyRemaining << (disabled && std::string(disabled) == "0" ? " (disabled)" : "") << '\n';
    if (disabled && std::string(disabled) == "0") {
        // Capture-only: keep the interpreter but still see every code image.
        VU1Interpreter::setTranslationLookup([](uint64_t hash, uint32_t, const uint8_t *code, uint32_t size)
                                                 -> VU1Interpreter::TranslatedProgram {
            captureImage(hash, code, size);
            return nullptr;
        });
        return;
    }
    VU1Interpreter::setTranslationLookup(lookupTranslation);
}
