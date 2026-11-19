#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NOUSER
#define NOGDI
#include <windows.h>
// ==========================================================================
// IMAGENS COMPRIMIDAS "ipu2" (fundos de menu, retratos, telas de carga)
// ==========================================================================
// O jogo guarda varias imagens grandes comprimidas no formato do IPU (o
// decodificador de MPEG do PS2). O fluxo original e:
//   1. 0x152560 (submit): o jogo escolhe uma imagem dentro de um arquivo
//      "ipu2" e manda o IPU decodificar via DMA.
//   2. 0x152840/0x152848 (wait): o jogo espera o IPU terminar.
// No port nao ha IPU de hardware. Entao:
//   - recordAndSubmit valida o cabecalho e anota a imagem pedida (jobs);
//   - waitForImage decodifica com o libavcodec (FFmpeg dentro do .exe),
//     grava os pixels na RAM no formato que o jogo espera e avisa o jogo
//     (interrupcao 3) como se o IPU tivesse terminado.
// Se algo nao bater (imagem invalida), o jogo para com "[Orochi:blocker]".
#include "native_image.h"
#include "native_ipu_decoder.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
// ImageJob: uma imagem a decodificar. source = endereco dos dados
// comprimidos na RAM, bytes = tamanho, width/height = dimensoes em pixels.
// jobs: imagens pedidas, indexadas pelo endereco de origem.
struct ImageJob { uint32_t source, bytes, width, height; };
PS2Runtime::RecompiledFunction submitOriginal{}, waitOriginal{};
std::unordered_map<uint32_t,ImageJob> jobs;
unsigned decodedFrames{};
std::atomic<uint64_t> perfDecodedFrames{};
std::atomic<uint64_t> perfTotalMicroseconds{};
std::atomic<uint64_t> perfProcessMicroseconds{};
std::atomic<uint64_t> perfCacheHits{};
// 2026-10-23: ipu2 frames are decoded in-process (libavcodec AV_CODEC_ID_IPU).
// OROCHI_IPU_DECODER=ffmpeg-cli restores the former external ffmpeg.exe path
// (reference/A-B only). OROCHI_IPU_VERIFY=N compares the first N in-process
// decodes byte-for-byte with the CLI output and logs [Orochi:ipu-verify].
bool useCliDecoder{};
unsigned verifyRemaining{};
std::unique_ptr<OrochiIpuDecoder> ipuDecoder;
// The game uploads the same ipu2 selection once per field buffer; reuse the
// last decoded pixels only when the whole payload and dimensions are identical.
// cache: a ultima imagem decodificada. O jogo pede a MESMA imagem uma vez
// para cada buffer de campo; se os bytes comprimidos e o tamanho forem
// iguais, reaproveitamos os pixels sem decodificar de novo.
struct DecodedCache { std::vector<uint8_t> payload,rgba; uint32_t width{},height{}; bool valid{}; } cache;
// inRam: true se [address, address+size) cabe inteiro nos 32 MB da RAM.
// word: le 4 bytes da RAM (lanca erro se o endereco for invalido).
// block: escreve o erro no log e para o jogo.
bool inRam(uint32_t address,uint64_t size) {
    return address<PS2_RAM_SIZE && size<=PS2_RAM_SIZE-address;
}
uint32_t word(uint8_t *ram,uint32_t address) {
    if(!inRam(address,4)) throw std::runtime_error("IPU metadata outside EE RAM");
    uint32_t value; std::memcpy(&value,ram+address,4); return value;
}
void block(PS2Runtime *runtime,const char *message) {
    std::cerr << "[Orochi:blocker] Native image decoder: " << message << '\n';
    runtime->requestStop();
}
// recordAndSubmit: substitui 0x152560.
// Argumentos do jogo: a0 = descritor do arquivo, a1 = numero da imagem.
// Formato "ipu2" validado aqui:
//   +0  assinatura "ipu2"   +4 tamanho total
//   +8  largura | altura<<16 (multiplos de 16, ate 1024)
//   +12 quantidade de imagens; depois, por imagem, (deslocamento, bytes)
// Exemplo: um fundo 640x448 -> dimensions = 640 | (448<<16).
// Anota a imagem em jobs e chama o submit original (que programa o DMA).
void recordAndSubmit(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime) {
    try {
        const auto asset=getRegU32(ctx,4)&0x1fffffffu;
        const auto selection=getRegU32(ctx,5);
        const auto data=word(ram,asset+8)&0x1fffffffu;
        if(word(ram,data)!=0x32757069u) throw std::runtime_error("unsupported image signature (expected ipu2)");
        const auto dimensions=word(ram,data+8), count=word(ram,data+12), size=word(ram,data+4);
        const auto width=dimensions&0xffffu, height=dimensions>>16;
        if(!width || !height || width>1024 || height>1024 || (width%16) || (height%16) ||
           selection>=count || count>10000 || size<16 || !inRam(data,size) ||
           16ull+count*8ull>size) throw std::runtime_error("invalid ipu2 header");
        const auto offset=word(ram,data+16+selection*8), bytes=word(ram,data+20+selection*8);
        if(offset>size || bytes>size-offset || !bytes) throw std::runtime_error("invalid ipu2 frame extent");
        jobs[data+offset]=ImageJob{data+offset,bytes,width,height};
        submitOriginal(ram,ctx,runtime);
    } catch(const std::exception &error) {
        std::cerr << "[Orochi:image-native] rejected submit a0=0x" << std::hex << getRegU32(ctx,4)
            << " a1=0x" << getRegU32(ctx,5) << " a2=0x" << getRegU32(ctx,6) << " ra=0x" << getRegU32(ctx,31);
        const auto asset=getRegU32(ctx,4)&0x1fffffffu;
        if(inRam(asset,16)) for(uint32_t i=0;i<16;i+=4) std::cerr << " [+" << i << "]=0x" << word(ram,asset+i);
        std::cerr << std::dec << '\n';
        block(runtime,error.what());
    }
}

// runDecoder: caminho ANTIGO, so para referencia (OROCHI_IPU_DECODER=
// ffmpeg-cli): chama o ffmpeg.exe externo. Lento (~58 ms por imagem).
void runDecoder(const std::filesystem::path &input,const std::filesystem::path &output,
                const std::filesystem::path &log) {
    wchar_t executable[32768]{};
    if(!SearchPathW(nullptr,L"ffmpeg.exe",nullptr,32768,executable,nullptr))
        throw std::runtime_error("FFmpeg with IPU decoder is required on PATH");
    auto quote=[](const std::wstring &value) { return L"\""+value+L"\""; };
    std::wstring command=quote(executable)+L" -nostdin -hide_banner -loglevel error -xerror -y -i "+
        quote(input.wstring())+L" -frames:v 1 -pix_fmt rgba -f rawvideo "+quote(output.wstring());
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
    HANDLE errors=CreateFileW(log.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&security,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    HANDLE nullInput=CreateFileW(L"NUL",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr);
    if(errors==INVALID_HANDLE_VALUE || nullInput==INVALID_HANDLE_VALUE) {
        if(errors!=INVALID_HANDLE_VALUE) CloseHandle(errors);
        if(nullInput!=INVALID_HANDLE_VALUE) CloseHandle(nullInput);
        throw std::runtime_error("could not open decoder diagnostic handles");
    }
    STARTUPINFOW startup{}; startup.cb=sizeof(startup); startup.dwFlags=STARTF_USESTDHANDLES;
    startup.hStdError=errors; startup.hStdOutput=errors; startup.hStdInput=nullInput;
    PROCESS_INFORMATION process{};
    const bool started=CreateProcessW(executable,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process);
    CloseHandle(errors); CloseHandle(nullInput);
    if(!started) throw std::runtime_error("could not start the native FFmpeg decoder");
    const auto state=WaitForSingleObject(process.hProcess,10000);
    if(state!=WAIT_OBJECT_0) { TerminateProcess(process.hProcess,1); WaitForSingleObject(process.hProcess,1000); }
    DWORD result=1; GetExitCodeProcess(process.hProcess,&result);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    if(state!=WAIT_OBJECT_0 || result!=0) throw std::runtime_error("FFmpeg decode failed; see local/native-image/decoder.log");
}

// Former (pre-2026-10-23) default path: stage an ipum file, spawn ffmpeg.exe,
// read raw RGBA back. Measured at ~58 ms/frame of process time; kept only for
// OROCHI_IPU_DECODER=ffmpeg-cli and OROCHI_IPU_VERIFY reference comparisons.
void decodeWithCli(const uint8_t *payload,uint32_t payloadBytes,uint32_t width,uint32_t height,
                   std::vector<uint8_t> &rgba) {
    const auto directory=PS2Runtime::getIoPaths().elfDirectory/L"native-image";
    std::filesystem::create_directories(directory);
    const auto input=directory/L"frame.ipu",output=directory/L"frame.rgba";
    const uint32_t header[4]={0x6d757069u,16u+payloadBytes,width|(height<<16),1u};
    { std::ofstream file(input,std::ios::binary); file.write(reinterpret_cast<const char *>(header),16);
      file.write(reinterpret_cast<const char *>(payload),payloadBytes);
      if(!file) throw std::runtime_error("could not stage the IPU frame"); }
    runDecoder(input,output,directory/L"decoder.log");
    const auto pixelBytes=size_t(width)*height*4u;
    if(std::filesystem::file_size(output)!=pixelBytes) throw std::runtime_error("decoded frame has an unexpected byte count");
    rgba.resize(pixelBytes);
    { std::ifstream file(output,std::ios::binary); file.read(reinterpret_cast<char *>(rgba.data()),rgba.size());
      if(!file) throw std::runtime_error("could not read decoded pixels"); }
}

// decodeActiveImage: faz o trabalho do IPU para a transferencia em curso.
//   1. Le os registradores de DMA do IPU (origem 0x1000b410, destino
//      0x1000b010, canal ativo bit 0x100 de 0x1000b000).
//   2. Acha a imagem anotada e confere se o destino comporta
//      largura x altura x 4 bytes (RGBA).
//   3. Corta o enchimento depois do fim de quadro (00 00 01 B0).
//   4. Decodifica (ou usa o cache).
//   5. Reordena os pixels: o IPU entrega blocos de 16x16 em sequencia, nao
//      linhas. Ex.: imagem 640 de largura -> o pixel (x=20, y=3) vai para o
//      bloco 1, linha 3, coluna 4 do bloco. Alfa fixo 0x80 (opaco no PS2).
//   6. Zera os contadores de DMA, desliga os canais e dispara a
//      interrupcao do IPU para o jogo continuar.
void decodeActiveImage(uint8_t *ram,PS2Runtime *runtime) {
    auto &memory=runtime->memory();
    const auto fromChcr=memory.readIORegister(0x1000b000u);
    if(!(fromChcr&0x100u)) return;
    const auto source=memory.readIORegister(0x1000b410u)&0x1fffffffu;
    const auto destination=memory.readIORegister(0x1000b010u)&0x1fffffffu;
    const auto found=jobs.find(source);
    if(found==jobs.end()) throw std::runtime_error("IPU transfer has no validated ipu2 frame");
    const auto &job=found->second;
    const auto pixelBytes=job.width*job.height*4u;
    if(!inRam(destination,pixelBytes) || memory.readIORegister(0x1000b020u)*16ull!=pixelBytes)
        throw std::runtime_error("IPU output transfer does not match the frame dimensions");
    const auto decodeStart=std::chrono::steady_clock::now();
    // ipu2 indexes independent IPU I-frames; ipum feeds the same stream to FFmpeg.
    // Exclude DMA alignment padding after the frame terminator.
    uint32_t payloadBytes=job.bytes;
    while(payloadBytes>=4 && std::memcmp(ram+source+payloadBytes-4,"\0\0\1\xb0",4)!=0) --payloadBytes;
    if(payloadBytes<4) throw std::runtime_error("IPU frame terminator is absent");
    const uint8_t *payload=ram+source;
    const bool cached=cache.valid && cache.width==job.width && cache.height==job.height &&
        cache.payload.size()==payloadBytes && std::memcmp(cache.payload.data(),payload,payloadBytes)==0;
    const auto procStart=std::chrono::steady_clock::now();
    if(!cached) {
        cache.valid=false;
        if(useCliDecoder) decodeWithCli(payload,payloadBytes,job.width,job.height,cache.rgba);
        else {
            if(!ipuDecoder) ipuDecoder=std::make_unique<OrochiIpuDecoder>();
            ipuDecoder->decode(payload,payloadBytes,job.width,job.height,cache.rgba);
        }
        if(cache.rgba.size()!=pixelBytes) throw std::runtime_error("decoded frame has an unexpected byte count");
        // OROCHI_IPU_DUMP=<folder>: each newly decoded frame as a binary PPM (diagnostics).
        if(const auto folder=std::getenv("OROCHI_IPU_DUMP")) {
            static unsigned dumped=0;
            std::ofstream file(std::filesystem::path(folder)/("ipu_"+std::to_string(dumped++)+".ppm"),std::ios::binary);
            file << "P6\n" << job.width << ' ' << job.height << "\n255\n";
            for(size_t i=0;i<cache.rgba.size();i+=4) file.write(reinterpret_cast<const char*>(cache.rgba.data()+i),3);
        }
        cache.payload.assign(payload,payload+payloadBytes);
        cache.width=job.width;cache.height=job.height;cache.valid=true;
    } else perfCacheHits.fetch_add(1u,std::memory_order_relaxed);
    const auto procEnd=std::chrono::steady_clock::now();
    if(!cached && !useCliDecoder && verifyRemaining) {
        --verifyRemaining;
        std::vector<uint8_t> reference;
        decodeWithCli(payload,payloadBytes,job.width,job.height,reference);
        size_t diff=0;int maxDelta=0;
        for(size_t i=0;i<reference.size() && i<cache.rgba.size();i++) if(reference[i]!=cache.rgba[i]) {
            ++diff;maxDelta=std::max(maxDelta,std::abs(int(reference[i])-int(cache.rgba[i])));
        }
        std::cout << "[Orochi:ipu-verify] source=0x" << std::hex << source << std::dec
            << " bytes=" << payloadBytes << " identical=" << (diff==0 && reference.size()==cache.rgba.size())
            << " diffBytes=" << diff << " maxDelta=" << maxDelta << '\n';
    }
    const auto &rgba=cache.rgba;
    // IPU output is an array of 16x16 RGBA macroblocks; GS upload packets arrange
    // those blocks on screen. The console's opaque alpha is 0x80, not 0xff.
    for(uint32_t y=0;y<job.height;y++) for(uint32_t x=0;x<job.width;x++) {
        const auto linear=(y*job.width+x)*4u;
        const auto tiled=(((y/16)*(job.width/16)+x/16)*256+(y%16)*16+x%16)*4u;
        std::memcpy(ram+destination+tiled,rgba.data()+linear,3);
        ram[destination+tiled+3]=0x80;
    }
    const auto decodeEnd=std::chrono::steady_clock::now();
    perfProcessMicroseconds.fetch_add(
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(procEnd-procStart).count()),
        std::memory_order_relaxed);
    perfTotalMicroseconds.fetch_add(
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(decodeEnd-decodeStart).count()),
        std::memory_order_relaxed);
    perfDecodedFrames.fetch_add(1u,std::memory_order_relaxed);
    memory.writeIORegister(0x1000b020u,0);
    memory.writeIORegister(0x1000b420u,0);
    memory.writeIORegister(0x1000b000u,fromChcr&~0x100u);
    memory.writeIORegister(0x1000b400u,memory.readIORegister(0x1000b400u)&~0x100u);
    // Notify the original game handler only after actual decoded pixels exist.
    runtime->eeScheduler().dispatchIrq(true,3u);
    if(decodedFrames++<8) std::cout << "[Orochi:image-native] source=0x" << std::hex << source
        << " destination=0x" << destination << std::dec << " pixels=" << pixelBytes << " completed=1\n";
}
// waitForImage: substitui a espera 0x152840/0x152848: decodifica e depois
// roda a espera original (que agora encontra o trabalho pronto).
void waitForImage(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime) {
    try { decodeActiveImage(ram,runtime); }
    catch(const std::exception &error) { block(runtime,error.what()); return; }
    waitOriginal(ram,ctx,runtime);
}
}

// installOrochiImageFunctions: chamada por main.cpp. Zera estado e
// estatisticas, escolhe o decodificador (padrao libavcodec) e troca as
// funcoes 0x152560, 0x152840 e 0x152848 do jogo.
// OROCHI_IPU_VERIFY=N compara as N primeiras imagens com o ffmpeg.exe.
bool installOrochiImageFunctions(PS2Runtime &runtime) {
    jobs.clear(); decodedFrames=0;
    perfDecodedFrames.store(0u,std::memory_order_relaxed);
    perfTotalMicroseconds.store(0u,std::memory_order_relaxed);
    perfProcessMicroseconds.store(0u,std::memory_order_relaxed);
    perfCacheHits.store(0u,std::memory_order_relaxed);
    cache=DecodedCache{}; ipuDecoder.reset();
    const char *mode=std::getenv("OROCHI_IPU_DECODER");
    useCliDecoder=mode && std::string(mode)=="ffmpeg-cli";
    const char *verify=std::getenv("OROCHI_IPU_VERIFY");
    verifyRemaining=verify?unsigned(std::max(0,std::min(1000,std::atoi(verify)))):0u;
    std::cout << "[Orochi:image-decoder] backend=" << (useCliDecoder?"ffmpeg-cli":"libavcodec")
        << " verifyFrames=" << verifyRemaining << '\n';
    submitOriginal=runtime.lookupFunction(0x152560u);
    waitOriginal=runtime.lookupFunction(0x152840u);
    return submitOriginal && waitOriginal && runtime.replaceFunction(0x152560u,recordAndSubmit) &&
        runtime.replaceFunction(0x152840u,waitForImage) && runtime.replaceFunction(0x152848u,waitForImage);
}

// getOrochiImagePerfSnapshot: estatisticas (imagens, tempo total, tempo de
// decodificacao, acertos de cache) para o log de desempenho.
OrochiImagePerfSnapshot getOrochiImagePerfSnapshot() {
    return {
        perfDecodedFrames.load(std::memory_order_relaxed),
        perfTotalMicroseconds.load(std::memory_order_relaxed),
        perfProcessMicroseconds.load(std::memory_order_relaxed),
        perfCacheHits.load(std::memory_order_relaxed)
    };
}
