// Export the VU1 interpreter's own decode of a microcode image, one JSON object
// per instruction pair, so tools/generate_vu1.py schedules translated code with
// exactly the latencies and register usage the interpreter enforces.
// usage: orochi_vu1_dump <vu1code.bin | vu0code.bin>   (16 KiB VU1 or 4 KiB VU0 image)
#include "runtime/ps2_vu1.h"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

struct VU1TranslationAccess
{
    static void printUsage(const char *name, const VU1Interpreter::InstructionUsage &usage)
    {
        std::printf("\"%s\":{\"vfRead\":[", name);
        for (uint32_t i = 0; i < usage.vfReadCount; ++i)
            std::printf("%s[%u,%u]", i ? "," : "", usage.vfRead[i].reg, usage.vfRead[i].lanes);
        std::printf("],\"vfWrite\":[%u,%u],\"viRead\":%u,\"viWrite\":%u,\"accRead\":%u,\"accWrite\":%u,"
                    "\"latency\":%u,\"vfLatency\":%u,\"viLatency\":%u,\"pipeline\":%u,\"waitQ\":%d,\"waitP\":%d,"
                    "\"readsClip\":%d,\"writesClip\":%d,\"delaysNextBranchRead\":%d,\"reserved\":%d}",
                    usage.vfWrite.reg, usage.vfWrite.lanes, usage.viRead, usage.viWrite, usage.accRead,
                    usage.accWrite, usage.latency, usage.vfLatency, usage.viLatency,
                    static_cast<unsigned>(usage.pipeline), usage.waitQ, usage.waitP, usage.readsClip,
                    usage.writesClip, usage.delaysNextBranchRead, usage.reserved);
    }

    static int dump(const std::vector<uint8_t> &code)
    {
        VU1Interpreter vu(code.size() == 0x1000u ? VU1Interpreter::Unit::VU0 : VU1Interpreter::Unit::VU1);
        std::printf("{\"pipelines\":{\"none\":%u,\"fmac\":%u,\"lsu\":%u,\"fdiv\":%u,\"efu\":%u,\"ialu\":%u,"
                    "\"branch\":%u,\"xgkick\":%u},\"fmacLatency\":%u,\"accForwardLatency\":%u}\n",
                    VU1Interpreter::PipelineNone, VU1Interpreter::PipelineFmac, VU1Interpreter::PipelineLsu,
                    VU1Interpreter::PipelineFdiv, VU1Interpreter::PipelineEfu, VU1Interpreter::PipelineIalu,
                    VU1Interpreter::PipelineBranch, VU1Interpreter::PipelineXgkick,
                    VU1Interpreter::kFmacLatency, VU1Interpreter::kAccForwardLatency);
        for (uint32_t pc = 0; pc + 8u <= code.size(); pc += 8u)
        {
            const VU1Interpreter::DecodedInstructionPair decoded = vu.decodeInstructionPair(code.data(), pc);
            std::printf("{\"pc\":%u,\"lower\":%u,\"upper\":%u,\"iBit\":%d,\"eBit\":%d,\"mBit\":%d,\"dBit\":%d,"
                        "\"tBit\":%d,\"upperVfShadowReg\":%u,\"suppressedLowerVf\":%u,",
                        pc, decoded.lower, decoded.upper, decoded.iBit, decoded.eBit, decoded.mBit,
                        decoded.dBit, decoded.tBit, decoded.upperVfShadowReg, decoded.suppressedLowerVf);
            printUsage("upperUsage", decoded.upperUsage);
            std::printf(",");
            printUsage("lowerUsage", decoded.lowerUsage);
            std::printf("}\n");
        }
        return 0;
    }
};

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<uint8_t> code((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (code.size() != 0x4000u && code.size() != 0x1000u)
    {
        std::cerr << "expected a 16 KiB VU1 or 4 KiB VU0 code image\n";
        return 3;
    }
    return VU1TranslationAccess::dump(code);
}
