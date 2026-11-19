// ==========================================================================
// RENDERIZADOR PROPRIO NA PLACA DE VIDEO (OROCHI_RENDERER=d3d11 ou auto)
// ==========================================================================
// Alternativa ao PCSX2: imita o GS do PS2 na GPU com compute shaders
// (codigo HLSL em gpu_raster_shader.h). Desenha na resolucao NATIVA do PS2
// (640x448), sem upscale e sem 16:9; o padrao do Setup e o PCSX2.
// Ideia central:
//   - A memoria de video do PS2 (4 MB) vira um buffer na GPU ("memory").
//   - Triangulos e sprites sao traduzidos (translate) e acumulados num lote.
//     A tela e dividida em quadrados de 16x16 pixels (tiles); cada tile
//     guarda quais primitivas o cobrem. Um dispatch desenha o lote inteiro,
//     um grupo de threads por tile, na ordem original das primitivas.
//   - O que a GPU nao sabe fazer cai no renderizador por CPU (fallback),
//     depois de copiar a VRAM de volta (download).
//   - O resultado precisa ser IDENTICO ao da CPU. OROCHI_GPU_VERIFY=N
//     compara as N primeiras primitivas com o desenho da CPU.
// Precisa de GPU Direct3D 11 com suporte a double nos shaders.
#include "native_gpu.h"
#include "gpu_raster_shader.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <map>
#include <memory>
#include <random>
#include <thread>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

namespace {
using Microsoft::WRL::ComPtr;
// vramBytes: tamanho da memoria do GS do PS2 (4 MB).
// maxPrims: primitivas por lote; tileGrid: 128x128 tiles de 16 px
// (cobre 2048x2048); clutSlots: versoes de paleta guardadas na GPU.
constexpr uint32_t vramBytes=4*1024*1024;
// Batches: primitives, tile references and CLUT versions in flight.
constexpr uint32_t maxPrims=32768,maxTileRefs=1u<<21,tileGrid=128,clutSlots=4096;
void checked(HRESULT result,const char *operation) {
    if(FAILED(result)) throw std::runtime_error(std::string(operation)+" HRESULT="+std::to_string(uint32_t(result)));
}
// Mirrors of the shader's DrawState and Prim (see gpu_raster_shader.h).
// GpuDrawState / GpuPrim: copias exatas das estruturas do shader (mesmos
// bytes). Se mudar uma, mude a outra em gpu_raster_shader.h; o
// static_assert abaixo confere os tamanhos (96 e 144 bytes).
// F_*: bits de opcoes por primitiva (blend, textura, filtro linear...).
// ByteRange: faixa de bytes da VRAM lida ou escrita (para achar conflitos).
struct GpuDrawState { uint32_t frame[4],zbuf[4],blend[4],tex[4],tex2[4],region[4]; };
struct GpuPrim { uint32_t head[4]; float a[4],b[4]; uint32_t v[3][4]; float w[3][4]; };
static_assert(sizeof(GpuDrawState)==96 && sizeof(GpuPrim)==144);
enum : uint32_t { F_ABE=1,F_PABE=2,F_FBA=4,F_FGE=8,F_TME=16,F_FST=32,F_IIP=64,F_LINEAR=128 };
struct ByteRange { uint64_t begin,end,layout; };
bool overlaps(const ByteRange &a,const ByteRange &b) { return a.begin<b.end && b.begin<a.end; }
float fabsQ(float q) { return std::fabs(q)>1.0e-8f ? q : 1.0f; }
bool isIndexed(uint32_t psm) { return psm==GS_PSM_T8||psm==GS_PSM_T8H||psm==GS_PSM_T4||psm==GS_PSM_T4HL||psm==GS_PSM_T4HH; }
bool gpuTextureFormat(uint32_t psm) {
    return psm==GS_PSM_CT32||psm==GS_PSM_CT24||psm==GS_PSM_CT16||psm==GS_PSM_CT16S||isIndexed(psm);
}
uint32_t pageRows(uint32_t psm,int lastRow) {
    const uint32_t format=psm&0x0Fu;
    const uint32_t height=(format==0u||format==1u) ? 32u : (format==4u ? 128u : 64u);
    return uint32_t(std::max(lastRow,0))/height+1u;
}
// GSMem::PixelStorageTraits layouts (ps2_gs_memory.cpp), for IMAGE transfers
// written on the GPU: the byte a pixel lands on and the bits it owns.
namespace layout {
constexpr uint8_t blocks32[32]={0,1,4,5,16,17,20,21,2,3,6,7,18,19,22,23,8,9,12,13,24,25,28,29,10,11,14,15,26,27,30,31};
constexpr uint8_t blocksZ32[32]={24,25,28,29,8,9,12,13,26,27,30,31,10,11,14,15,16,17,20,21,0,1,4,5,18,19,22,23,2,3,6,7};
constexpr uint8_t blocksC16[32]={0,2,8,10,1,3,9,11,4,6,12,14,5,7,13,15,16,18,24,26,17,19,25,27,20,22,28,30,21,23,29,31};
constexpr uint8_t blocksC16S[32]={0,2,16,18,1,3,17,19,8,10,24,26,9,11,25,27,4,6,20,22,5,7,21,23,12,14,28,30,13,15,29,31};
constexpr uint8_t blocksZ16[32]={24,26,16,18,25,27,17,19,28,30,20,22,29,31,21,23,8,10,0,2,9,11,1,3,12,14,4,6,13,15,5,7};
constexpr uint8_t blocksZ16S[32]={24,26,8,10,25,27,9,11,16,18,0,2,17,19,1,3,28,30,12,14,29,31,13,15,20,22,4,6,21,23,5,7};
constexpr uint8_t blocksP4[32]={0,2,8,10,1,3,9,11,4,6,12,14,5,7,13,15,16,18,24,26,17,19,25,27,20,22,28,30,21,23,29,31};
constexpr uint16_t columns32[64]={0,1,4,5,8,9,12,13,2,3,6,7,10,11,14,15,16,17,20,21,24,25,28,29,18,19,22,23,26,27,30,31,
    32,33,36,37,40,41,44,45,34,35,38,39,42,43,46,47,48,49,52,53,56,57,60,61,50,51,54,55,58,59,62,63};
constexpr uint16_t columns16[128]={0,2,8,10,16,18,24,26,1,3,9,11,17,19,25,27,4,6,12,14,20,22,28,30,5,7,13,15,21,23,29,31,
    32,34,40,42,48,50,56,58,33,35,41,43,49,51,57,59,36,38,44,46,52,54,60,62,37,39,45,47,53,55,61,63,
    64,66,72,74,80,82,88,90,65,67,73,75,81,83,89,91,68,70,76,78,84,86,92,94,69,71,77,79,85,87,93,95,
    96,98,104,106,112,114,120,122,97,99,105,107,113,115,121,123,100,102,108,110,116,118,124,126,101,103,109,111,117,119,125,127};
constexpr uint16_t columns8[256]={0,4,16,20,32,36,48,52,2,6,18,22,34,38,50,54,8,12,24,28,40,44,56,60,10,14,26,30,42,46,58,62,
    33,37,49,53,1,5,17,21,35,39,51,55,3,7,19,23,41,45,57,61,9,13,25,29,43,47,59,63,11,15,27,31,
    96,100,112,116,64,68,80,84,98,102,114,118,66,70,82,86,104,108,120,124,72,76,88,92,106,110,122,126,74,78,90,94,
    65,69,81,85,97,101,113,117,67,71,83,87,99,103,115,119,73,77,89,93,105,109,121,125,75,79,91,95,107,111,123,127,
    128,132,144,148,160,164,176,180,130,134,146,150,162,166,178,182,136,140,152,156,168,172,184,188,138,142,154,158,170,174,186,190,
    161,165,177,181,129,133,145,149,163,167,179,183,131,135,147,151,169,173,185,189,137,141,153,157,171,175,187,191,139,143,155,159,
    224,228,240,244,192,196,208,212,226,230,242,246,194,198,210,214,232,236,248,252,200,204,216,220,234,238,250,254,202,206,218,222,
    193,197,209,213,225,229,241,245,195,199,211,215,227,231,243,247,201,205,217,221,233,237,249,253,203,207,219,223,235,239,251,255};
constexpr uint16_t columns4[512]={
    0,8,32,40,64,72,96,104,2,10,34,42,66,74,98,106,4,12,36,44,68,76,100,108,6,14,38,46,70,78,102,110,
    16,24,48,56,80,88,112,120,18,26,50,58,82,90,114,122,20,28,52,60,84,92,116,124,22,30,54,62,86,94,118,126,
    65,73,97,105,1,9,33,41,67,75,99,107,3,11,35,43,69,77,101,109,5,13,37,45,71,79,103,111,7,15,39,47,
    81,89,113,121,17,25,49,57,83,91,115,123,19,27,51,59,85,93,117,125,21,29,53,61,87,95,119,127,23,31,55,63,
    192,200,224,232,128,136,160,168,194,202,226,234,130,138,162,170,196,204,228,236,132,140,164,172,198,206,230,238,134,142,166,174,
    208,216,240,248,144,152,176,184,210,218,242,250,146,154,178,186,212,220,244,252,148,156,180,188,214,222,246,254,150,158,182,190,
    129,137,161,169,193,201,225,233,131,139,163,171,195,203,227,235,133,141,165,173,197,205,229,237,135,143,167,175,199,207,231,239,
    145,153,177,185,209,217,241,249,147,155,179,187,211,219,243,251,149,157,181,189,213,221,245,253,151,159,183,191,215,223,247,255,
    256,264,288,296,320,328,352,360,258,266,290,298,322,330,354,362,260,268,292,300,324,332,356,364,262,270,294,302,326,334,358,366,
    272,280,304,312,336,344,368,376,274,282,306,314,338,346,370,378,276,284,308,316,340,348,372,380,278,286,310,318,342,350,374,382,
    321,329,353,361,257,265,289,297,323,331,355,363,259,267,291,299,325,333,357,365,261,269,293,301,327,335,359,367,263,271,295,303,
    337,345,369,377,273,281,305,313,339,347,371,379,275,283,307,315,341,349,373,381,277,285,309,317,343,351,375,383,279,287,311,319,
    448,456,480,488,384,392,416,424,450,458,482,490,386,394,418,426,452,460,484,492,388,396,420,428,454,462,486,494,390,398,422,430,
    464,472,496,504,400,408,432,440,466,474,498,506,402,410,434,442,468,476,500,508,404,412,436,444,470,478,502,510,406,414,438,446,
    385,393,417,425,449,457,481,489,387,395,419,427,451,459,483,491,389,397,421,429,453,461,485,493,391,399,423,431,455,463,487,495,
    401,409,433,441,465,473,497,505,403,411,435,443,467,475,499,507,405,413,437,445,469,477,501,509,407,415,439,447,471,479,503,511};
// Format: como um formato de pixel do PS2 e arrumado na VRAM. O GS nao
// guarda imagens linha a linha: divide em paginas (ex.: 64x32 pixels em
// 32 bits), blocos e colunas embaralhados (tabelas acima). pixelWrite
// calcula em qual palavra de 32 bits e em quais bits um pixel cai.
// Exemplo: CT32, x=0,y=0 -> pagina 0, bloco 0, palavra 0, mascara 0xffffffff.
struct Format { uint32_t pageW,pageH,colW,colH; const uint8_t *blocks; const uint16_t *columns; uint32_t unpackedBits,bitOffset,packedBytes; };
// Formats this backend writes natively; nullptr when the CPU must do it.
inline const Format *format(uint32_t psm) {
    static const Format c32{64,32,8,8,blocks32,columns32,32,0,4},z32{64,32,8,8,blocksZ32,columns32,32,0,4},
        c16{64,64,16,8,blocksC16,columns16,16,0,2},c16s{64,64,16,8,blocksC16S,columns16,16,0,2},
        z16{64,64,16,8,blocksZ16,columns16,16,0,2},z16s{64,64,16,8,blocksZ16S,columns16,16,0,2},
        p8{128,64,16,16,blocks32,columns8,8,0,1},p4{128,128,32,16,blocksP4,columns4,4,0,1},
        p8h{64,32,8,8,blocks32,columns32,32,24,1},p4hl{64,32,8,8,blocks32,columns32,32,24,1},p4hh{64,32,8,8,blocks32,columns32,32,28,1};
    switch(psm) {
    case GS_PSM_CT32:case GS_PSM_CT24:return &c32;
    case GS_PSM_Z32:case GS_PSM_Z24:return &z32;
    case GS_PSM_CT16:return &c16;case GS_PSM_CT16S:return &c16s;case GS_PSM_Z16:return &z16;case GS_PSM_Z16S:return &z16s;
    case GS_PSM_T8:return &p8;case GS_PSM_T4:return &p4;case GS_PSM_T8H:return &p8h;case GS_PSM_T4HL:return &p4hl;case GS_PSM_T4HH:return &p4hh;
    default:return nullptr;
    }
}
// The block and column tables in one array for the GPU, and each table's offset in it.
inline const std::vector<uint32_t> &tables(std::map<const void *,uint32_t> *offsets=nullptr) {
    static std::map<const void *,uint32_t> where;
    static const std::vector<uint32_t> all=[] {
        std::vector<uint32_t> out;
        auto add=[&](const auto &table) { where[table]=uint32_t(out.size());out.insert(out.end(),std::begin(table),std::end(table)); };
        add(blocks32);add(blocksZ32);add(blocksC16);add(blocksC16S);add(blocksZ16);add(blocksZ16S);add(blocksP4);
        add(columns32);add(columns16);add(columns8);add(columns4);
        return out;
    }();
    if(offsets) *offsets=where;
    return all;
}
// PixelStorageTraits::Write: the 32-bit word, the value placed in it and the bits replaced.
inline void pixelWrite(const Format &f,uint32_t psm,uint32_t base,uint32_t bw,uint32_t x,uint32_t y,uint32_t value,
                       uint32_t &word,uint32_t &bits,uint32_t &mask) {
    const uint32_t page=base/32+(y/f.pageH)*((bw*64)/f.pageW)+x/f.pageW;
    const uint32_t blockCols=f.pageW/f.colW,blockRows=f.pageH/f.colH;
    const uint32_t block=f.blocks[((y/f.colH)%blockRows)*blockCols+(x/f.colW)%blockCols];
    const uint32_t lx=x%f.colW,ly=y%f.colH;
    const uint32_t pixel=page*(f.pageW*f.pageH)+((base%32)+block)*(f.colW*f.colH)+f.columns[ly*f.colW+lx];
    const uint32_t bitAddress=pixel*f.unpackedBits+f.bitOffset;
    const uint32_t byte=(bitAddress/8)&(vramBytes-f.packedBytes);
    const uint32_t shift=(byte&3)*8+bitAddress%8;
    uint32_t width=f.packedBytes*8;
    if(psm==GS_PSM_CT24||psm==GS_PSM_Z24) width=24;
    else if(psm==GS_PSM_T4||psm==GS_PSM_T4HL||psm==GS_PSM_T4HH) width=4;
    mask=(width==32 ? 0xffffffffu : ((1u<<width)-1u))<<shift;
    word=byte&~3u;bits=(value<<shift)&mask;
}
}

// OROCHI_GPU_TIMING: adds the microseconds of a scope to *total (when set).
// Span: mede o tempo de um trecho (OROCHI_GPU_TIMING=1) e soma em *total.
struct Span {
    double *total;std::chrono::steady_clock::time_point start;
    explicit Span(double *t):total(t) { if(total) start=std::chrono::steady_clock::now(); }
    ~Span() { if(total) *total+=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count(); }
};
struct GpuTiming { double wait{},copy{},convert{},image{},submit{},transfer{}; uint64_t presents{},readbacks{},pixels{}; };

// One run of IMAGE units uploaded raw: the GPU applies layout::pixelWrite to
// each (see uploadRaw in gpu_raster_shader.h for the fields).
// Envio "cru" de imagens (IMAGE do GS): em vez de calcular o endereco de
// cada pixel na CPU, os bytes vao direto para a GPU e o shader uploadRaw
// aplica o mesmo calculo de pixelWrite. RawSegment descreve um trecho;
// RawRange e o retangulo que uma transferencia pode escrever.
// conflicts(): duas transferencias na mesma fila nao podem escrever os
// mesmos bits (os videos mandam centenas de blocos por quadro, entao
// retangulos separados do mesmo buffer sao permitidos).
struct RawSegment { uint32_t a[4],b[4],c[4],d[4]; };
constexpr uint32_t rawCapacity=4*1024*1024,maxRawSegments=4096,maxRawGroups=65535;
// What a raw transfer may write: its pages, and its rectangle in its buffer.
struct RawRange {
    uint64_t begin,end,id;
    uint32_t dbp,dbw,psm,x0,y0,x1,y1;
    bool inRow;  // every pixel left of the buffer width in pages (no page row overlap)
};
// Queued transfers run in one dispatch, in any order: they must not write the
// same bits. Disjoint rectangles of one buffer and format never do (movies
// upload hundreds of macroblocks into the same pages every frame).
bool conflicts(const RawRange &a,const RawRange &b) {
    if(a.id==b.id || a.end<=b.begin || b.end<=a.begin) return false;
    const bool apart=a.x1<=b.x0 || b.x1<=a.x0 || a.y1<=b.y0 || b.y1<=a.y0;
    return !(a.inRow && b.inRow && a.dbp==b.dbp && a.dbw==b.dbw && a.psm==b.psm && apart);
}

struct GpuStats { uint64_t triangles{},sprites{},batches{},tileRefs{},fallbacks{},uploads{},readbacks{},
    clutLoads{},verified{},mismatches{},imagePixels{},imageDispatches{}; };

// OrochiGpuBackend: o renderizador. Mantem DUAS copias da VRAM:
//   host (na RAM, usada pela CPU e pelo jogo) e memory (na GPU).
//   cpuDirty = a copia da CPU mudou e precisa subir (upload).
//   gpuDirty = a GPU desenhou e a CPU precisa baixar (download).
// Toda funcao publica trava "mutex" (pode ser chamada pela thread de
// desenho e pela thread do jogo).
class OrochiGpuBackend final : public GSRasterBackend {
public:
    // Construtor: cria o dispositivo Direct3D 11 de HARDWARE (nunca o WARP,
    // que desenharia na CPU), confere suporte a double, compila os 4 compute
    // shaders (drawBatch, uploadImage, loadClut, uploadRaw) e cria os buffers.
    // Lanca excecao se algo falhar; main.cpp entao usa o renderizador da CPU.
    OrochiGpuBackend() {
        profileFallbacks=std::getenv("OROCHI_GPU_PROFILE")!=nullptr;
        timed=std::getenv("OROCHI_GPU_TIMING")!=nullptr;
        if(const auto value=std::getenv("OROCHI_GPU_VERIFY"))
            verifyRemaining=std::min(100000ul,std::strtoul(value,nullptr,10));
        // Primitives drawn before verification starts (to reach a scene).
        if(const auto value=std::getenv("OROCHI_GPU_VERIFY_SKIP")) verifySkip=std::strtoull(value,nullptr,10);
        const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL actual{};
        // HARDWARE only: WARP would still rasterize on the processor.
        checked(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,levels,1,
                D3D11_SDK_VERSION,&device,&actual,&context),"Create hardware D3D11 device");
        D3D11_FEATURE_DATA_DOUBLES doubles{};
        checked(device->CheckFeatureSupport(D3D11_FEATURE_DOUBLES,&doubles,sizeof(doubles)),"Query double support");
        // Exact division and depth interpolation need double precision.
        if(!doubles.DoublePrecisionFloatShaderOps) throw std::runtime_error("GPU lacks double-precision shaders");
        ComPtr<IDXGIDevice> dxgi; checked(device.As(&dxgi),"Query DXGI device");
        ComPtr<IDXGIAdapter> adapter; checked(dxgi->GetAdapter(&adapter),"Query GPU adapter");
        DXGI_ADAPTER_DESC description{}; checked(adapter->GetDesc(&description),"Query GPU description");
        std::string name;
        for(const auto ch:description.Description) { if(!ch) break; name+=ch<128?char(ch):'?'; }
        std::cout << "[Orochi:gpu] hardware=D3D11 adapter=" << name
            << " feature=11.0 scope=tiled-triangles-sprites-clut-and-image-uploads\n";
        auto compile=[&](const char *entry,ComPtr<ID3D11ComputeShader> &out) {
            ComPtr<ID3DBlob> shader,error;
            const auto compiled=D3DCompile(orochiGpuRasterShader,sizeof(orochiGpuRasterShader)-1,"orochi-gs",nullptr,nullptr,
                entry,"cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_IEEE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&shader,&error);
            if(FAILED(compiled) && error)
                throw std::runtime_error(std::string(static_cast<const char *>(error->GetBufferPointer()),error->GetBufferSize()));
            checked(compiled,"Compile GS compute shader");
            checked(device->CreateComputeShader(shader->GetBufferPointer(),shader->GetBufferSize(),nullptr,&out),"Create GS compute shader");
        };
        compile("drawBatch",drawShader);compile("uploadImage",imageShader);compile("loadClut",clutShader);
        compile("uploadRaw",rawShader);
        D3D11_BUFFER_DESC desc{}; desc.ByteWidth=vramBytes; desc.Usage=D3D11_USAGE_DEFAULT;
        desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS; desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        checked(device->CreateBuffer(&desc,nullptr,&memory),"Create GPU GS memory");
        desc.ByteWidth=clutSlots*1024;
        checked(device->CreateBuffer(&desc,nullptr,&clutBuffer),"Create GPU CLUT versions");
        D3D11_UNORDERED_ACCESS_VIEW_DESC view{}; view.Format=DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension=D3D11_UAV_DIMENSION_BUFFER; view.Buffer.NumElements=vramBytes/4;
        view.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;
        checked(device->CreateUnorderedAccessView(memory.Get(),&view,&uav),"Create GS memory UAV");
        view.Buffer.NumElements=clutSlots*256;
        checked(device->CreateUnorderedAccessView(clutBuffer.Get(),&view,&clutUav),"Create CLUT UAV");
        desc.ByteWidth=vramBytes;desc.BindFlags=0; desc.MiscFlags=0; desc.Usage=D3D11_USAGE_STAGING; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        checked(device->CreateBuffer(&desc,nullptr,&staging),"Create GS readback");
        desc.ByteWidth=1024;
        checked(device->CreateBuffer(&desc,nullptr,&clutStaging),"Create CLUT readback");
        auto structured=[&](uint32_t stride,uint32_t count,ComPtr<ID3D11Buffer> &buffer,ComPtr<ID3D11ShaderResourceView> &srv) {
            D3D11_BUFFER_DESC d{};d.ByteWidth=stride*count;d.Usage=D3D11_USAGE_DYNAMIC;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            d.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.StructureByteStride=stride;
            checked(device->CreateBuffer(&d,nullptr,&buffer),"Create GS batch buffer");
            D3D11_SHADER_RESOURCE_VIEW_DESC v{};v.Format=DXGI_FORMAT_UNKNOWN;v.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;
            v.Buffer.NumElements=count;
            checked(device->CreateShaderResourceView(buffer.Get(),&v,&srv),"Create GS batch view");
        };
        structured(sizeof(GpuDrawState),maxPrims,stateBuffer,stateSrv);
        structured(sizeof(GpuPrim),maxPrims,primBuffer,primSrv);
        structured(16,tileGrid*tileGrid,tileBuffer,tileSrv);
        structured(4,maxTileRefs,indexBuffer,indexSrv);
        desc={};desc.ByteWidth=65536*4*sizeof(uint32_t);desc.Usage=D3D11_USAGE_DYNAMIC;
        desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        checked(device->CreateBuffer(&desc,nullptr,&imageBuffer),"Create GS image input buffer");
        D3D11_SHADER_RESOURCE_VIEW_DESC imageView{};imageView.Format=DXGI_FORMAT_R32G32B32A32_UINT;
        imageView.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;imageView.Buffer.NumElements=65536;
        checked(device->CreateShaderResourceView(imageBuffer.Get(),&imageView,&imageSrv),"Create GS image input view");
        // Raw IMAGE bytes (plus a word read past the end), their segments,
        // the segment of every thread group, and the layout tables.
        auto typed=[&](uint32_t words,D3D11_USAGE usage,const void *initial,ComPtr<ID3D11Buffer> &buffer,
                       ComPtr<ID3D11ShaderResourceView> &srv) {
            D3D11_BUFFER_DESC d{};d.ByteWidth=words*4;d.Usage=usage;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            d.CPUAccessFlags=usage==D3D11_USAGE_DYNAMIC ? D3D11_CPU_ACCESS_WRITE : 0;
            D3D11_SUBRESOURCE_DATA data{initial,0,0};
            checked(device->CreateBuffer(&d,initial?&data:nullptr,&buffer),"Create GS raw upload buffer");
            D3D11_SHADER_RESOURCE_VIEW_DESC v{};v.Format=DXGI_FORMAT_R32_UINT;v.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;
            v.Buffer.NumElements=words;
            checked(device->CreateShaderResourceView(buffer.Get(),&v,&srv),"Create GS raw upload view");
        };
        typed(rawCapacity/4+4,D3D11_USAGE_DYNAMIC,nullptr,rawBuffer,rawSrv);
        typed(maxRawGroups,D3D11_USAGE_DYNAMIC,nullptr,rawGroupBuffer,rawGroupSrv);
        std::map<const void *,uint32_t> offsets;const auto &all=layout::tables(&offsets);
        typed(uint32_t(all.size()),D3D11_USAGE_IMMUTABLE,all.data(),layoutBuffer,layoutSrv);
        structured(sizeof(RawSegment),maxRawSegments,rawSegmentBuffer,rawSegmentSrv);
        rawBytes.reserve(rawCapacity+8);rawSegments.reserve(maxRawSegments);rawGroups.reserve(maxRawGroups);
        desc={};desc.ByteWidth=32;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        checked(device->CreateBuffer(&desc,nullptr,&imageConstants),"Create GS upload parameters");
        desc.ByteWidth=48;
        checked(device->CreateBuffer(&desc,nullptr,&clutConstants),"Create CLUT load parameters");
        D3D11_QUERY_DESC query{};query.Query=D3D11_QUERY_EVENT;
        checked(device->CreateQuery(&query,&finishQuery),"Create GS completion fence");
        pendingImage.reserve(65536);pendingImageWords.assign(vramBytes/4,0u);
        bins.resize(tileGrid*tileGrid);
        prims.reserve(maxPrims);states.reserve(maxPrims);
        clearCluts();
    }
    ~OrochiGpuBackend() override {
        std::cout << "[Orochi:gpu-summary] triangles=" << stats.triangles << " sprites=" << stats.sprites
            << " batches=" << stats.batches << " tileRefs=" << stats.tileRefs << " cpuFallbacks=" << stats.fallbacks
            << " clutLoads=" << stats.clutLoads << " uploads=" << stats.uploads << " readbacks=" << stats.readbacks
            << " verified=" << stats.verified << " mismatches=" << stats.mismatches
            << " nativeImagePixels=" << stats.imagePixels << " imageDispatches=" << stats.imageDispatches << '\n';
        for(const auto &[key,value]:fallbackAreas)
            std::cout << "[Orochi:gpu-fallback] key=" << key << " calls=" << value << '\n';
    }
    GpuStats counters() const { std::lock_guard lock(mutex); return stats; }
    void Initialize(uint8_t *vram,uint32_t size) override {
        if(!vram || size!=vramBytes) throw std::invalid_argument("D3D11 GS requires exactly 4 MiB external VRAM");
        std::lock_guard lock(mutex);discardBatch();host=vram;cpu.Initialize(vram,size);gpuDirty=false;cpuDirty=true;nativeTransfer=false;
        pendingImage.clear();std::fill(pendingImageWords.begin(),pendingImageWords.end(),0u);clutCbp.fill(0);clearCluts();
        rawBytes.clear();rawSegments.clear();rawGroups.clear();rawRanges.clear();
    }
    void Reset() override {
        std::lock_guard lock(mutex);download();cpu.Reset();cpuDirty=true;nativeTransfer=false;clutCbp.fill(0);
        clutSlot=0;cpuClutSlot=0;
    }
    // Submit: recebe UMA primitiva (triangulo ou sprite) do jogo.
    //   1. translate(): converte para o formato da GPU. Pode pedir Skip (nada
    //      a desenhar) ou Fallback (desenhar na CPU).
    //   2. Se a primitiva le/escreve algo que o lote atual ainda vai escrever
    //      de outro jeito (ex.: usa como textura o que o lote desenhou), o lote
    //      e desenhado antes (flushBatch).
    //   3. Adiciona a primitiva ao lote e a cada tile de 16x16 que ela cobre.
    //      Ex.: sprite de (0,0) a (40,20) -> tiles x 0..2, y 0..1 = 6 tiles.
    void Submit(const GSPrimitiveBatch &batch) override {
        std::lock_guard lock(mutex);
        Span span(timed?&timing.submit:nullptr);
        if(!host) return;
        GpuPrim prim{};GpuDrawState state{};ByteRange frame{},depth{};std::array<ByteRange,2> texture{};
        const auto result=translate(batch,prim,state,frame,depth,texture);
        if(result==Translation::Skip) return;
        if(result==Translation::Fallback) { fallback(batch);return; }
        const bool sampled=texture[0].end!=texture[0].begin;
        bool conflict=prims.size()==maxPrims;
        for(const ByteRange &written:queueWrites) {
            conflict=conflict||(overlaps(frame,written)&&written.layout!=frame.layout)||
                (overlaps(depth,written)&&written.layout!=depth.layout);
            for(const ByteRange &read:texture) conflict=conflict||(sampled&&overlaps(read,written));
        }
        for(const ByteRange &read:queueReads) conflict=conflict||overlaps(read,frame)||overlaps(read,depth);
        const uint32_t x0=prim.head[2]&0xffff,y0=prim.head[2]>>16,x1=prim.head[3]&0xffff,y1=prim.head[3]>>16;
        const uint32_t refs=((x1>>4)-(x0>>4)+1)*((y1>>4)-(y0>>4)+1);
        conflict=conflict||tileRefs+refs>maxTileRefs;
        if(conflict) flushBatch();
        upload();flushImageWrites();
        std::vector<uint8_t> reference;
        if(verifyRemaining && stats.triangles+stats.sprites>=verifySkip) reference=referenceDraw(batch);
        if(prims.empty()) batchFirstSlot=clutSlot;
        state.blend[2]=clutSlot;
        if(states.empty() || std::memcmp(&states.back(),&state,sizeof(state))!=0) states.push_back(state);
        prim.head[1]=uint32_t(states.size()-1);
        const uint32_t index=uint32_t(prims.size());prims.push_back(prim);
        for(uint32_t ty=y0>>4;ty<=(y1>>4);++ty) for(uint32_t tx=x0>>4;tx<=(x1>>4);++tx) {
            auto &bin=bins[ty*tileGrid+tx];
            if(bin.empty()) touched.push_back(ty*tileGrid+tx);
            bin.push_back(index);
        }
        tileRefs+=refs;
        addRange(queueWrites,frame);addRange(queueWrites,depth);
        if(sampled) for(const ByteRange &read:texture) if(read.end!=read.begin) addRange(queueReads,read);
        if(prim.head[0]==0) ++stats.triangles; else ++stats.sprites;
        if(!reference.empty()) compareReference(batch,reference);
        if((stats.triangles+stats.sprites)%200000==0)
            std::cout << "[Orochi:gpu-draw] triangles=" << stats.triangles << " sprites=" << stats.sprites
                << " batches=" << stats.batches << " cpuFallbacks=" << stats.fallbacks << " readbacks=" << stats.readbacks << '\n';
    }
    // LoadClut: carrega uma paleta (CLUT) de texturas indexadas (4/8 bits).
    // Cada carga vira uma nova "versao" (slot) na GPU, assim primitivas ja no
    // lote continuam usando a paleta antiga. As regras de quando carregar
    // (campo CLD) sao as mesmas do renderizador da CPU.
    void LoadClut(const GSTex0Reg &tex,const GSTexClutReg &clut) override {
        // GSCpuBackend::LoadClut: which TEX0 writes load a palette.
        if(!isIndexed(tex.psm) || tex.cld==0 || tex.cld>5) return;
        std::lock_guard lock(mutex);
        if(tex.cld==2) clutCbp[0]=tex.cbp;
        if(tex.cld==3) clutCbp[1]=tex.cbp;
        if(tex.cld==4 || tex.cld==5) {
            auto &bank=clutCbp[tex.cld-4];
            if(bank==tex.cbp) return;
            bank=tex.cbp;
        }
        const bool sixteen=tex.cpsm==GS_PSM_CT16||tex.cpsm==GS_PSM_CT16S;
        if(!host || (!sixteen && tex.cpsm!=GS_PSM_CT32 && tex.cpsm!=GS_PSM_CT24)) return;
        // The palette is read from VRAM now: queued primitives that write
        // it must run first (CSM2 offsets are not bounded here).
        const uint64_t source=uint64_t(tex.cbp)*256u;
        const ByteRange palette{source,source+2u*8192u,0};
        bool written=tex.csm!=0;
        for(const ByteRange &range:queueWrites) written=written||overlaps(palette,range);
        uint32_t next=clutSlot+1>=clutSlots ? 1u : clutSlot+1;
        if(written || (!prims.empty() && next==batchFirstSlot)) flushBatch();
        upload();flushImageWrites();
        std::vector<uint8_t> before;std::array<uint16_t,512> previous{};
        const bool verify=verifyRemaining!=0 && stats.triangles+stats.sprites>=verifySkip;
        if(verify) { download();before.assign(host,host+vramBytes);previous=readClut(clutSlot); }
        const uint32_t params[12]={clutSlot,next,tex.cbp,tex.cpsm, tex.csm,tex.csa,
            uint32_t(tex.psm==GS_PSM_T4||tex.psm==GS_PSM_T4HL||tex.psm==GS_PSM_T4HH),clut.cbw, clut.cou,clut.cov,0,0};
        context->UpdateSubresource(clutConstants.Get(),0,nullptr,params,0,0);
        auto *cb=clutConstants.Get();ID3D11UnorderedAccessView *targets[2]={uav.Get(),clutUav.Get()};
        context->CSSetShader(clutShader.Get(),nullptr,0);context->CSSetConstantBuffers(2,1,&cb);
        context->CSSetUnorderedAccessViews(0,2,targets,nullptr);
        context->Dispatch(4,1,1);
        targets[0]=targets[1]=nullptr;context->CSSetUnorderedAccessViews(0,2,targets,nullptr);
        clutSlot=next;++stats.clutLoads;
        if(verify) {
            if(!verifier) verifier=std::make_unique<GSCpuBackend>();
            verifier->Initialize(before.data(),vramBytes);verifier->SetClutTable(previous);
            GSTex0Reg load=tex;load.cld=1;verifier->LoadClut(load,clut);
            if(verifier->GetClutTable()!=readClut(clutSlot)) {
                ++stats.mismatches;
                std::cout << "[Orochi:gpu-verify] CLUT MISMATCH cpsm=" << unsigned(tex.cpsm) << " csm=" << unsigned(tex.csm)
                    << " csa=" << unsigned(tex.csa) << " psm=" << unsigned(tex.psm) << '\n';
            }
        }
    }
    // BeginTransfer: inicio de uma copia de imagem para a VRAM (textura,
    // quadro de video). Formatos suportados vao direto para a GPU
    // (nativeTransfer); os outros, e leituras da VRAM, vao para a CPU.
    void BeginTransfer(const GSTransferCommand &command) override {
        std::lock_guard lock(mutex);
        Span span(timed?&timing.transfer:nullptr);
        // IMAGE writes are dispatched separately; queued primitives go first.
        flushBatch();
        const auto &b=command.bitbltbuf;const auto &p=command.trxpos;const auto &r=command.trxreg;
        // Host-to-local IMAGE in any format GSMem writes: addresses wrap at
        // 4 MiB exactly like the CPU's, so no extent check is needed.
        const uint32_t dbw=std::max<uint32_t>(b.dbw,1u);
        nativeTransfer=command.direction==0 && layout::format(b.dpsm) && dbw<=32 && r.rrw && r.rrh &&
            uint32_t(p.dsax)+r.rrw<=dbw*64 && uint32_t(p.dsay)+r.rrh<=2048;
        if(nativeTransfer) {
            transfer=command;transferPixels=uint32_t(r.rrw)*r.rrh;
            transferState={p.dsax,p.dsay,transferPixels,0,0,0};
            // Raw upload needs every pixel of the rectangle on its own bits: no
            // two page rows sharing pages (odd widths of 128-pixel pages) and no
            // wrap at 4 MiB. Its pages bound what it writes.
            const auto &f=*layout::format(b.dpsm);
            const uint32_t perRow=(dbw*64)/f.pageW,row0=p.dsay/f.pageH,row1=(uint32_t(p.dsay)+r.rrh-1)/f.pageH;
            const uint64_t first=b.dbp/32+uint64_t(row0)*perRow+p.dsax/f.pageW;
            const uint64_t last=b.dbp/32+uint64_t(row1)*perRow+(uint32_t(p.dsax)+r.rrw-1)/f.pageW+1;
            ++transferId;
            const bool inRow=uint32_t(p.dsax)+r.rrw<=perRow*f.pageW;
            rawTransfer=(inRow || row0==row1) && (last+1)*8192<=vramBytes;
            transferRange={first*8192,(last+1)*8192,transferId,b.dbp,dbw,b.dpsm,p.dsax,p.dsay,
                           uint32_t(p.dsax)+r.rrw,uint32_t(p.dsay)+r.rrh,inRow};
        } else {download();cpu.BeginTransfer(command);cpuDirty=true;}
    }
    // UploadImage: os bytes da imagem iniciada em BeginTransfer (pode chegar
    // em pedacos). Cada pixel e colocado no endereco certo da VRAM; com
    // rawTransfer, o calculo de endereco e feito na GPU (uploadRaw).
    void UploadImage(const uint8_t *data,uint32_t size) override {
        std::lock_guard lock(mutex);
        Span span(timed?&timing.image:nullptr);
        timing.pixels+=size/4;
        flushBatch();
        if(!nativeTransfer) {download();cpu.UploadImage(data,size);cpuDirty=true;return;}
        if(!data || transferState.direction!=0 || !host) return;
        upload();
        // GSCpuBackend::UploadImage: whole pixels of the format; a trailing
        // partial pixel is dropped, T4 packs two pixels per byte.
        const auto &b=transfer.bitbltbuf;const uint32_t psm=b.dpsm,dbw=std::max<uint32_t>(b.dbw,1u);
        const auto &f=*layout::format(psm);
        const uint32_t rrw=transfer.trxreg.rrw;
        auto put=[&](uint32_t position,uint32_t value) {
            const uint32_t x=transfer.trxpos.dsax+position%rrw,y=transfer.trxpos.dsay+position/rrw;
            if(psm==GS_PSM_CT32) {
                // GS frontend's own CT32 addressing, as before.
                queueImageWrite(GSPSMCT32::addrPSMCT32(b.dbp,dbw,x,y),value,0xffffffffu);
                return;
            }
            uint32_t word,bits,mask;layout::pixelWrite(f,psm,b.dbp,dbw,x,y,value,word,bits,mask);
            queueImageWrite(word,bits,mask);
        };
        const uint32_t unit=(psm==GS_PSM_CT32||psm==GS_PSM_Z32) ? 4u : (psm==GS_PSM_CT24||psm==GS_PSM_Z24) ? 3u :
            (f.unpackedBits==16 ? 2u : 1u);
        const bool fourBit=psm==GS_PSM_T4||psm==GS_PSM_T4HL||psm==GS_PSM_T4HH;
        if(rawTransfer) { uploadRaw(data,size,f,unit,fourBit);return; }
        if(!rawSegments.empty()) flushImageWrites();
        uint32_t offset=0,lastFirst=transferState.copiedPixels;
        while(offset+unit<=size && transferState.copiedPixels<transferPixels) {
            const uint32_t first=transferState.copiedPixels;lastFirst=first;
            if(fourBit) {
                put(first,data[offset]&15u);
                if(first+1<transferPixels) put(first+1,(data[offset]>>4)&15u);
                transferState.copiedPixels+=std::min<uint32_t>(2u,transferPixels-first);
            } else {
                uint32_t value=0;std::memcpy(&value,data+offset,unit);
                put(first,value);++transferState.copiedPixels;
            }
            offset+=unit;stats.imagePixels+=fourBit?2:1;gpuDirty=true;
        }
        const bool finished=transferState.copiedPixels==transferPixels;
        // On completion the CPU keeps the position of the last unit's first pixel.
        const auto position=finished ? lastFirst : transferState.copiedPixels;
        transferState.x=transfer.trxpos.dsax+position%transfer.trxreg.rrw;
        transferState.y=transfer.trxpos.dsay+position/transfer.trxreg.rrw;
        if(finished) {transferState.direction=3;transferState.totalPixels=0;}
    }
    void Flush() override { std::lock_guard lock(mutex);flushBatch();flushImageWrites();context->Flush(); }
    void TextureFlush() override { std::lock_guard lock(mutex);cpu.TextureFlush(); }
    // Sync: FINISH espera a GPU terminar (ate 5 s, senao erro); outros
    // motivos (exceto apresentar quadro) baixam a VRAM para a CPU.
    void Sync(GSSyncReason reason) override {
        std::lock_guard lock(mutex);
        if(reason==GSSyncReason::Finish) {
            // FINISH needs completion, not a CPU copy of all 4 MiB of VRAM.
            flushBatch();flushImageWrites();context->End(finishQuery.Get());context->Flush();
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            HRESULT result;
            while((result=context->GetData(finishQuery.Get(),nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH))==S_FALSE) {
                if(std::chrono::steady_clock::now()>=deadline) throw std::runtime_error("GS GPU completion timed out");
                std::this_thread::yield();
            }
            checked(result,"Wait for GS completion");
        } else if(reason!=GSSyncReason::Presentation) download();
    }
    // Present: monta o quadro para a tela. Baixa a VRAM e usa a conversao da
    // CPU (framebuffer do PS2 -> imagem RGBA). Com OROCHI_GPU_TIMING imprime
    // tempos medios a cada 240 quadros.
    PresentationFrame Present(const GSPresentationRequest &request) override {
        std::lock_guard lock(mutex);
        download();
        PresentationFrame frame;
        { Span span(timed?&timing.convert:nullptr);frame=cpu.Present(request); }
        if(timed && ++timing.presents==240) {
            const double n=double(timing.presents);
            std::cout << "[Orochi:gpu-timing] per present us: wait=" << timing.wait/n << " copy=" << timing.copy/n
                << " convert=" << timing.convert/n << " image=" << timing.image/n << " transfer=" << timing.transfer/n
                << " submit=" << timing.submit/n << " readbacks=" << double(timing.readbacks)/n
                << " imageWords=" << double(timing.pixels)/n << '\n';
            // With OROCHI_GPU_PROFILE, the CPU fallbacks of this interval.
            for(const auto &[key,value]:fallbackAreas)
                if(const auto reported=fallbacksReported[key];value!=reported)
                    std::cout << "[Orochi:gpu-fallback-interval] calls=" << value-reported << " key=" << key << '\n';
            fallbacksReported=fallbackAreas;
            timing={};
        }
        return frame;
    }
    bool ClearFramebuffer(const GSContext &frame,uint32_t value) override {
        std::lock_guard lock(mutex);download();const auto result=cpu.ClearFramebuffer(frame,value);cpuDirty=true;return result;
    }
    uint32_t ConsumeLocalToHostBytes(uint8_t *out,uint32_t size) override {
        std::lock_guard lock(mutex);if(nativeTransfer)return 0;download();return cpu.ConsumeLocalToHostBytes(out,size);
    }
    uint32_t ReadVram(uint32_t psm,uint32_t base,uint32_t bw,uint32_t x,uint32_t y) const override {
        std::lock_guard lock(mutex);download();return cpu.ReadVram(psm,base,bw,x,y);
    }
    void WriteVram(uint32_t psm,uint32_t base,uint32_t bw,uint32_t x,uint32_t y,uint32_t value) override {
        std::lock_guard lock(mutex);download();cpu.WriteVram(psm,base,bw,x,y,value);cpuDirty=true;
    }
    void SnapshotVram(std::vector<uint8_t> &out) const override {
        std::lock_guard lock(mutex);download();cpu.SnapshotVram(out);
    }
    GSTransferSnapshot GetTransferSnapshot() const override {
        std::lock_guard lock(mutex);return nativeTransfer ? transferState : cpu.GetTransferSnapshot();
    }
private:
    enum class Translation { Draw, Skip, Fallback };
    // Builds the GPU form of a primitive with GSCpuBackend's own arithmetic
    // (bounding boxes, edge setup, sprite texel spans), plus the byte ranges
    // it reads and writes.
    // translate: converte uma primitiva do GS para GpuPrim/GpuDrawState com a
    // MESMA aritmetica do renderizador da CPU (caixa envolvente, bordas,
    // coordenadas de textura dos sprites). Tambem devolve as faixas de VRAM
    // que ela escreve (frame, depth) e le (texture).
    // Retorna Draw (vai para a GPU), Skip (fora da tela) ou Fallback (algo
    // nao suportado; rejectLine guarda a linha do motivo para o perfil).
    Translation translate(const GSPrimitiveBatch &batch,GpuPrim &p,GpuDrawState &d,ByteRange &frame,
                          ByteRange &depth,std::array<ByteRange,2> &texture) const {
        auto reject=[&](unsigned line) {rejectLine=line;return Translation::Fallback;};
        const auto &s=batch.state; const auto &c=s.context;
        const bool triangle=s.prim.type==GS_PRIM_TRIANGLE||s.prim.type==GS_PRIM_TRISTRIP||s.prim.type==GS_PRIM_TRIFAN;
        if(!(triangle&&batch.vertexCount==3) && !(s.prim.type==GS_PRIM_SPRITE&&batch.vertexCount==2)) return reject(__LINE__);
        if(c.frame.psm>1 || c.frame.fbw>32) return reject(__LINE__);
        if(c.scissor.x0>c.scissor.x1 || c.scissor.y0>c.scissor.y1) return Translation::Skip;
        const uint32_t fbw=std::max<uint32_t>(c.frame.fbw,1u);
        for(unsigned i=0;i<batch.vertexCount;++i) {
            const auto &v=batch.vertices[i];
            if(!std::isfinite(v.x)||!std::isfinite(v.y)||!std::isfinite(v.z)||std::abs(v.x)>65535.f||std::abs(v.y)>65535.f)
                return reject(__LINE__);
        }
        const int ofx=c.xyoffset.ofx>>4,ofy=c.xyoffset.ofy>>4;
        int x0,y0,x1,y1;
        if(triangle) {
            const auto &v0=batch.vertices[0],&v1=batch.vertices[1],&v2=batch.vertices[2];
            // GSCpuBackend::DrawTriangle setup.
            const float fx0=v0.x-float(ofx),fy0=v0.y-float(ofy),fx1=v1.x-float(ofx),fy1=v1.y-float(ofy);
            const float fx2=v2.x-float(ofx),fy2=v2.y-float(ofy);
            x0=std::clamp(int(std::floor(std::min({fx0,fx1,fx2}))),int(c.scissor.x0),int(c.scissor.x1));
            x1=std::clamp(int(std::ceil(std::max({fx0,fx1,fx2}))),int(c.scissor.x0),int(c.scissor.x1));
            y0=std::clamp(int(std::floor(std::min({fy0,fy1,fy2}))),int(c.scissor.y0),int(c.scissor.y1));
            y1=std::clamp(int(std::ceil(std::max({fy0,fy1,fy2}))),int(c.scissor.y0),int(c.scissor.y1));
            const float denom=(fy1-fy2)*(fx0-fx2)+(fx2-fx1)*(fy0-fy2);
            if(std::fabs(denom)<0.001f) return Translation::Skip;
            p.head[0]=0;
            p.a[0]=fy1-fy2;p.a[1]=fx2-fx1;p.a[2]=fy2-fy0;p.a[3]=fx0-fx2;
            p.b[0]=fx2;p.b[1]=fy2;p.b[2]=denom<0.0f?-1.0f:1.0f;p.b[3]=1.0f/std::fabs(denom);
            for(unsigned i=0;i<3;++i) {
                const auto &v=batch.vertices[i];
                uint64_t z;std::memcpy(&z,&v.z,8);
                p.v[i][0]=uint32_t(z);p.v[i][1]=uint32_t(z>>32);
                p.v[i][2]=v.r|(uint32_t(v.g)<<8)|(uint32_t(v.b)<<16)|(uint32_t(v.a)<<24);
                p.v[i][3]=v.u|(uint32_t(v.v)<<16);
                p.w[i][0]=v.s;p.w[i][1]=v.t;p.w[i][2]=v.q;p.w[i][3]=float(v.fog);
            }
        } else {
            const auto &v0=batch.vertices[0],&v1=batch.vertices[1];
            // GSCpuBackend::DrawSprite setup.
            int sx0=int(v0.x)-ofx,sy0=int(v0.y)-ofy,sx1=int(v1.x)-ofx,sy1=int(v1.y)-ofy;
            if(sx0>sx1) std::swap(sx0,sx1);
            if(sy0>sy1) std::swap(sy0,sy1);
            const int spanX=std::max(1,sx1-sx0),spanY=std::max(1,sy1-sy0);
            const int ux1=sx0+spanX-1,uy1=sy0+spanY-1;
            if(ux1<c.scissor.x0||sx0>c.scissor.x1||uy1<c.scissor.y0||sy0>c.scissor.y1) return Translation::Skip;
            x0=std::clamp(sx0,int(c.scissor.x0),int(c.scissor.x1));x1=std::clamp(ux1,int(c.scissor.x0),int(c.scissor.x1));
            y0=std::clamp(sy0,int(c.scissor.y0),int(c.scissor.y1));y1=std::clamp(uy1,int(c.scissor.y0),int(c.scissor.y1));
            p.head[0]=1;
            if(s.prim.tme) {
                if(s.prim.fst) { p.a[0]=float(v0.u>>4);p.a[1]=float(v0.v>>4);p.a[2]=float(v1.u>>4);p.a[3]=float(v1.v>>4); }
                else {
                    const float q0=fabsQ(v0.q),q1=fabsQ(v1.q);
                    p.a[0]=(v0.s/q0)*float(s.textureWidth);p.a[1]=(v0.t/q0)*float(s.textureHeight);
                    p.a[2]=(v1.s/q1)*float(s.textureWidth);p.a[3]=(v1.t/q1)*float(s.textureHeight);
                }
            }
            const int32_t geometry[4]={sx0,sy0,spanX,spanY};std::memcpy(p.b,geometry,16);
            p.v[0][0]=static_cast<uint32_t>(v1.z);
            p.v[0][2]=v1.r|(uint32_t(v1.g)<<8)|(uint32_t(v1.b)<<16)|(uint32_t(v1.a)<<24);
            p.w[0][3]=float(v1.fog);
        }
        // Distinct pixels must own distinct words: no wrapping past the buffer width or VRAM.
        if(x0<0||y0<0||x1>=int(fbw*64)||y1>2047) return reject(__LINE__);
        p.head[2]=uint32_t(x0)|(uint32_t(y0)<<16);p.head[3]=uint32_t(x1)|(uint32_t(y1)<<16);
        // GSCpuBackend::PrimitiveRanges, bounded by the primitive's last row rather
        // than the scissor (full-screen passes run with a 2048-row scissor).
        auto layout=[&](uint32_t psm,uint32_t base) { return (uint64_t(psm)<<48)|(uint64_t(fbw)<<32)|base; };
        const uint64_t frameBegin=uint64_t(c.frame.fbp)*8192;
        frame={frameBegin,frameBegin+pageRows(c.frame.psm,y1)*fbw*8192,layout(c.frame.psm,c.frame.fbp)};
        const uint32_t ztest=uint32_t((c.test>>17)&3u);
        if(frame.end>vramBytes) return reject(__LINE__);
        depth={0,0,0};
        if(ztest>=2u || !c.zbuf.zmask) {
            const uint32_t zpsm=c.zbuf.psm;
            if(zpsm!=GS_PSM_Z32&&zpsm!=GS_PSM_Z24&&zpsm!=GS_PSM_Z16&&zpsm!=GS_PSM_Z16S) return reject(__LINE__);
            const uint64_t depthBegin=uint64_t(c.zbuf.zbp)*8192;
            depth={depthBegin,depthBegin+pageRows(zpsm,y1)*fbw*8192,layout(zpsm|0x100u,c.zbuf.zbp)};
            if(depth.end>vramBytes || overlaps(frame,depth)) return reject(__LINE__);
        }
        texture={};
        if(s.prim.tme) {
            const auto &t=c.tex0;
            if(!gpuTextureFormat(t.psm) || !s.textureWidth || !s.textureHeight) return reject(__LINE__);
            // Bytes any texel lookup can reach: the largest coordinate each
            // wrap mode yields, through the format's page layout. A block
            // offset inside the first page reaches at most two pages further.
            auto largest=[&](uint32_t mode,uint32_t size,uint32_t low,uint32_t high) -> uint64_t {
                return mode<2u ? size-1u : (mode==2u ? high : (low|high));
            };
            uint64_t maxU=largest(uint32_t(c.clamp&3u),s.textureWidth,uint32_t((c.clamp>>4)&1023),uint32_t((c.clamp>>14)&1023));
            uint64_t maxV=largest(uint32_t((c.clamp>>2)&3u),s.textureHeight,uint32_t((c.clamp>>24)&1023),uint32_t((c.clamp>>34)&1023));
            uint64_t minU=0,minV=0;
            if(p.head[0]==1) {
                // DrawSprite interpolates strictly between the two texel
                // coordinates; add the bilinear neighbour and rounding. Screen
                // effects copy a buffer region into another region of the same
                // pages, which the whole-texture bounds would call an overlap.
                auto spriteSpan=[](uint32_t mode,uint32_t size,float a,float b,uint64_t &first,uint64_t &last) {
                    if(!std::isfinite(a) || !std::isfinite(b)) return;
                    const float low=std::min(a,b),high=std::max(a,b);
                    if(high>=65536.0f || low<=-65536.0f) return;
                    const int64_t from=int64_t(std::floor(low))-2,reach=int64_t(std::ceil(high))+2,top=int64_t(size)-1;
                    if(mode==1u) { first=uint64_t(std::clamp<int64_t>(from,0,top));last=uint64_t(std::clamp<int64_t>(reach,0,top)); }
                    // Repeat: coordinates past either edge wrap to the other.
                    else if(mode==0u && from>=0 && reach<=top) { first=uint64_t(from);last=uint64_t(reach); }
                };
                spriteSpan(uint32_t(c.clamp&3u),s.textureWidth,p.a[0],p.a[2],minU,maxU);
                spriteSpan(uint32_t((c.clamp>>2)&3u),s.textureHeight,p.a[1],p.a[3],minV,maxV);
            }
            const uint32_t format=t.psm&0x0Fu;
            const uint64_t pageWidth=(t.psm==GS_PSM_T8||t.psm==GS_PSM_T4) ? 128u : 64u;
            const uint64_t pageHeight=(format==0u||format==1u||t.psm==GS_PSM_T8H||t.psm==GS_PSM_T4HL||t.psm==GS_PSM_T4HH) ? 32u :
                (t.psm==GS_PSM_T4 ? 128u : 64u);
            const uint64_t stride=uint64_t(t.tbw)*64u/pageWidth;
            // Rows above minV are never read (block offsets only move texels later).
            const uint64_t begin=uint64_t(t.tbp0)*256u+(minV/pageHeight)*stride*8192u;
            const uint64_t end=(uint64_t(t.tbp0/32u)+(maxV/pageHeight)*stride+maxU/pageWidth+3u)*8192u;
            texture[0]={begin,std::min<uint64_t>(end,vramBytes),0};
            if(end>vramBytes) texture[1]={0,std::min<uint64_t>(end-vramBytes,vramBytes),0};
            // A primitive sampling what it draws depends on pixel order.
            for(const ByteRange &read:texture)
                if(read.end!=read.begin && (overlaps(read,frame)||overlaps(read,depth))) return reject(__LINE__);
        }
        d.frame[0]=c.frame.fbp*32;d.frame[1]=fbw;d.frame[2]=c.frame.psm;d.frame[3]=c.frame.fbmsk;
        d.zbuf[0]=c.zbuf.zbp*32;d.zbuf[1]=c.zbuf.psm;d.zbuf[2]=c.zbuf.zmask;d.zbuf[3]=uint32_t(c.test);
        d.blend[0]=uint32_t(c.alpha&255)|(uint32_t((c.alpha>>32)&255)<<8);
        d.blend[1]=(s.prim.abe?F_ABE:0)|(s.pabe?F_PABE:0)|((c.fba&1)?F_FBA:0)|(s.prim.fge?F_FGE:0)|
            (s.prim.tme?F_TME:0)|(s.prim.fst?F_FST:0)|(s.prim.iip?F_IIP:0)|(s.linearFilter?F_LINEAR:0);
        d.blend[3]=s.fogR|(uint32_t(s.fogG)<<8)|(uint32_t(s.fogB)<<16);
        if(s.prim.tme) {
            const auto &t=c.tex0;
            d.tex[0]=t.tbp0;d.tex[1]=t.tbw;d.tex[2]=t.psm;
            d.tex[3]=(t.tfx&3u)|(uint32_t(t.tcc&1u)<<2)|(uint32_t(t.cpsm&63u)<<8)|(uint32_t(t.csm&1u)<<16)|(uint32_t(t.csa&31u)<<24);
            d.tex2[0]=s.textureWidth;d.tex2[1]=s.textureHeight;
            d.tex2[2]=s.texa.ta0|(uint32_t(s.texa.ta1)<<8)|(uint32_t(s.texa.aem)<<16);
            d.tex2[3]=uint32_t(c.clamp&15u);
            d.region[0]=uint32_t((c.clamp>>4)&1023);d.region[1]=uint32_t((c.clamp>>14)&1023);
            d.region[2]=uint32_t((c.clamp>>24)&1023);d.region[3]=uint32_t((c.clamp>>34)&1023);
        }
        return Translation::Draw;
    }
    static void addRange(std::vector<ByteRange> &ranges,const ByteRange &range) {
        if(range.end==range.begin) return;
        for(const ByteRange &known:ranges)
            if(known.begin==range.begin && known.end==range.end && known.layout==range.layout) return;
        ranges.push_back(range);
    }
    // fallback: desenha a primitiva na CPU. Antes desenha o lote da GPU e
    // baixa a VRAM, para manter a ordem. Com OROCHI_GPU_PROFILE=1 conta e
    // registra no log os tipos de primitiva que cairam aqui.
    void fallback(const GSPrimitiveBatch &batch) {
        if(profileFallbacks && fallbackAreas.size()<512) {
            const auto &s=batch.state; const auto &c=s.context;
            // Draw-state shape only (no buffer addresses), to size GPU support.
            const std::string key=std::to_string(rejectLine)+" type="+std::to_string(unsigned(s.prim.type))+
                " tme="+std::to_string(s.prim.tme)+" tpsm="+std::to_string(c.tex0.psm)+
                " cpsm="+std::to_string(c.tex0.cpsm)+" fpsm="+std::to_string(c.frame.psm)+" zpsm="+std::to_string(c.zbuf.psm)+
                " test="+std::to_string(uint32_t(c.test))+" zmask="+std::to_string(c.zbuf.zmask)+
                " fbw="+std::to_string(c.frame.fbw)+" fst="+std::to_string(s.prim.fst);
            // The first few of each kind in full, to see what they read and write.
            if(++fallbackAreas[key]<=6) {
                const auto &v0=batch.vertices[0],&v1=batch.vertices[1];
                std::cout << "[Orochi:gpu-fallback-sample] line=" << rejectLine << " fbp=" << c.frame.fbp << " fbw=" << c.frame.fbw
                    << " fbmsk=0x" << std::hex << c.frame.fbmsk << std::dec << " zbp=" << c.zbuf.zbp
                    << " tbp0=" << c.tex0.tbp0 << " tbw=" << unsigned(c.tex0.tbw) << " tw=" << s.textureWidth
                    << " th=" << s.textureHeight << " clamp=0x" << std::hex << c.clamp << " alpha=0x" << c.alpha
                    << " test=0x" << c.test << std::dec << " abe=" << s.prim.abe << " lin=" << s.linearFilter
                    << " ofx=" << (c.xyoffset.ofx>>4) << " ofy=" << (c.xyoffset.ofy>>4)
                    << " scissor=" << c.scissor.x0 << ',' << c.scissor.y0 << ',' << c.scissor.x1 << ',' << c.scissor.y1
                    << " v0=" << v0.x << ',' << v0.y << " uv0=" << v0.u/16.0 << ',' << v0.v/16.0
                    << " v1=" << v1.x << ',' << v1.y << " uv1=" << v1.u/16.0 << ',' << v1.v/16.0
                    << " rgba=" << unsigned(v1.r) << ',' << unsigned(v1.g) << ',' << unsigned(v1.b) << ',' << unsigned(v1.a) << '\n';
            }
        }
        flushBatch();download();syncCpuClut();cpu.Submit(batch);cpuDirty=true;++stats.fallbacks;
    }
    void discardBatch() {
        for(const uint32_t tile:touched) bins[tile].clear();
        touched.clear();prims.clear();states.clear();queueWrites.clear();queueReads.clear();tileRefs=0;
    }
    // flushBatch: desenha o lote acumulado na GPU. Monta a lista de tiles e,
    // para cada tile, os indices das primitivas que o cobrem (na ordem),
    // envia os buffers e faz UM Dispatch (um grupo de threads por tile).
    void flushBatch() const {
        if(prims.empty()) return;
        auto write=[&](ID3D11Buffer *buffer,const void *data,size_t bytes) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            checked(context->Map(buffer,0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"Map GS batch");
            std::memcpy(mapped.pData,data,bytes);context->Unmap(buffer,0);
        };
        tileList.clear();indexList.clear();
        for(const uint32_t tile:touched) {
            auto &bin=bins[tile];
            tileList.push_back({tile%tileGrid,tile/tileGrid,uint32_t(indexList.size()),uint32_t(bin.size())});
            indexList.insert(indexList.end(),bin.begin(),bin.end());
            bin.clear();
        }
        write(stateBuffer.Get(),states.data(),states.size()*sizeof(GpuDrawState));
        write(primBuffer.Get(),prims.data(),prims.size()*sizeof(GpuPrim));
        write(tileBuffer.Get(),tileList.data(),tileList.size()*16);
        write(indexBuffer.Get(),indexList.data(),indexList.size()*4);
        ID3D11ShaderResourceView *views[4]={stateSrv.Get(),primSrv.Get(),tileSrv.Get(),indexSrv.Get()};
        ID3D11UnorderedAccessView *targets[2]={uav.Get(),clutUav.Get()};
        context->CSSetShader(drawShader.Get(),nullptr,0);
        context->CSSetShaderResources(2,4,views);context->CSSetUnorderedAccessViews(0,2,targets,nullptr);
        context->Dispatch(uint32_t(tileList.size()),1,1);
        for(auto &view:views) view=nullptr;
        targets[0]=targets[1]=nullptr;
        context->CSSetShaderResources(2,4,views);context->CSSetUnorderedAccessViews(0,2,targets,nullptr);
        ++stats.batches;stats.tileRefs+=indexList.size();gpuDirty=true;
        touched.clear();prims.clear();states.clear();queueWrites.clear();queueReads.clear();tileRefs=0;
    }
    // upload: se a copia da CPU mudou (cpuDirty), envia os 4 MB para a GPU.
    void upload() const {
        if(!host || !cpuDirty) return;
        // CPU fallbacks may still be queued for parallel bands.
        cpu.Flush();
        context->UpdateSubresource(memory.Get(),0,nullptr,host,0,0);cpuDirty=false;++stats.uploads;
    }
    // One entry per word and dispatch; a later write to the same word merges
    // its bits in, keeping the IMAGE order.
    // queueImageWrite: guarda a escrita de um pixel (palavra, valor, bits).
    // Duas escritas na mesma palavra viram uma so, respeitando a ordem.
    // A cada 65536 palavras pendentes, envia para a GPU (flushImageWrites).
    void queueImageWrite(uint32_t word,uint32_t value,uint32_t mask) const {
        uint32_t &slot=pendingImageWords[word/4];
        if(slot) {
            auto &entry=pendingImage[slot-1];
            entry[1]=(entry[1]&~mask)|(value&mask);entry[2]|=mask;
            return;
        }
        pendingImage.push_back({word,value&mask,mask,0u});slot=uint32_t(pendingImage.size());
        if(pendingImage.size()==65536) flushImageWrites();
    }
    // The same progress as the per-pixel loop in UploadImage, with the
    // addressing left to the GPU (uploadRaw).
    void uploadRaw(const uint8_t *data,uint32_t size,const layout::Format &f,uint32_t unit,bool fourBit) {
        if(!pendingImage.empty()) flushImageWrites();
        // Earlier transfers still queued may write the same pages.
        // Every new transfer is checked against the whole list: keep it short.
        if(rawRanges.size()>=512) flushImageWrites();
        if(rawRanges.empty() || rawRanges.back().id!=transferRange.id)
            for(const auto &range:rawRanges)
                if(conflicts(range,transferRange)) { flushImageWrites();break; }
        const uint32_t perUnit=fourBit?2u:1u;
        uint32_t units=std::min(size/unit,(transferPixels-transferState.copiedPixels+perUnit-1)/perUnit);
        uint32_t offset=0,lastFirst=transferState.copiedPixels;
        static std::map<const void *,uint32_t> offsets;
        if(offsets.empty()) layout::tables(&offsets);
        const auto &b=transfer.bitbltbuf;
        const uint32_t width=(b.dpsm==GS_PSM_CT24||b.dpsm==GS_PSM_Z24) ? 24u : fourBit ? 4u : f.packedBytes*8;
        while(units) {
            uint32_t room=uint32_t(std::min<uint64_t>((rawCapacity-rawBytes.size())/unit,uint64_t(maxRawGroups-rawGroups.size())*64));
            if(!room || rawSegments.size()==maxRawSegments) { flushImageWrites();continue; }
            const uint32_t count=std::min(units,room),first=transferState.copiedPixels;
            if(rawRanges.empty() || rawRanges.back().id!=transferRange.id) rawRanges.push_back(transferRange);
            RawSegment s{{b.dbp,std::max<uint32_t>(b.dbw,1u),transfer.trxpos.dsax,transfer.trxpos.dsay},
                {transfer.trxreg.rrw,first,transferPixels,uint32_t(rawBytes.size())},
                {unit,perUnit,count,uint32_t(rawGroups.size())},
                {f.pageW|(f.pageH<<16),f.colW|(f.colH<<16),offsets[f.blocks]|(offsets[f.columns]<<16),
                 f.unpackedBits|(f.bitOffset<<8)|(f.packedBytes<<16)|(width<<24)}};
            rawGroups.insert(rawGroups.end(),(count+63)/64,uint32_t(rawSegments.size()));
            rawSegments.push_back(s);
            rawBytes.insert(rawBytes.end(),data+offset,data+offset+size_t(count)*unit);
            lastFirst=first+(count-1)*perUnit;
            transferState.copiedPixels=std::min(transferPixels,first+count*perUnit);
            offset+=count*unit;units-=count;stats.imagePixels+=uint64_t(count)*perUnit;gpuDirty=true;
        }
        const bool finished=transferState.copiedPixels==transferPixels;
        const auto position=finished ? lastFirst : transferState.copiedPixels;
        transferState.x=transfer.trxpos.dsax+position%transfer.trxreg.rrw;
        transferState.y=transfer.trxpos.dsay+position/transfer.trxreg.rrw;
        if(finished) {transferState.direction=3;transferState.totalPixels=0;}
    }
    void flushImageWrites() const {
        flushRawImages();
        if(pendingImage.empty()) return;
        upload();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        checked(context->Map(imageBuffer.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"Map GS image input");
        std::memcpy(mapped.pData,pendingImage.data(),pendingImage.size()*16);context->Unmap(imageBuffer.Get(),0);
        const uint32_t params[8]={0,0,0,0,0,0,uint32_t(pendingImage.size()),0};
        context->UpdateSubresource(imageConstants.Get(),0,nullptr,params,0,0);
        auto *cb=imageConstants.Get();auto *source=imageSrv.Get();auto *target=uav.Get();
        context->CSSetShader(imageShader.Get(),nullptr,0);context->CSSetConstantBuffers(1,1,&cb);
        context->CSSetShaderResources(1,1,&source);context->CSSetUnorderedAccessViews(0,1,&target,nullptr);
        context->Dispatch((uint32_t(pendingImage.size())+63)/64,1,1);
        target=nullptr;source=nullptr;
        context->CSSetUnorderedAccessViews(0,1,&target,nullptr);context->CSSetShaderResources(1,1,&source);
        for(const auto &write:pendingImage) pendingImageWords[write[0]/4]=0;
        pendingImage.clear();++stats.imageDispatches;gpuDirty=true;
    }
    void flushRawImages() const {
        if(rawSegments.empty()) return;
        upload();
        auto write=[&](ID3D11Buffer *buffer,const void *source,size_t bytes) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            checked(context->Map(buffer,0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"Map GS raw upload");
            std::memcpy(mapped.pData,source,bytes);context->Unmap(buffer,0);
        };
        rawBytes.insert(rawBytes.end(),8,0);  // the shader reads one word past a unit
        write(rawBuffer.Get(),rawBytes.data(),rawBytes.size());
        write(rawSegmentBuffer.Get(),rawSegments.data(),rawSegments.size()*sizeof(RawSegment));
        write(rawGroupBuffer.Get(),rawGroups.data(),rawGroups.size()*4);
        ID3D11ShaderResourceView *views[4]={rawSegmentSrv.Get(),rawGroupSrv.Get(),rawSrv.Get(),layoutSrv.Get()};
        auto *target=uav.Get();
        context->CSSetShader(rawShader.Get(),nullptr,0);
        context->CSSetShaderResources(6,4,views);context->CSSetUnorderedAccessViews(0,1,&target,nullptr);
        context->Dispatch(uint32_t(rawGroups.size()),1,1);
        for(auto &view:views) view=nullptr;
        target=nullptr;
        context->CSSetShaderResources(6,4,views);context->CSSetUnorderedAccessViews(0,1,&target,nullptr);
        rawBytes.clear();rawSegments.clear();rawGroups.clear();rawRanges.clear();
        ++stats.imageDispatches;gpuDirty=true;
    }
    // download: desenha o que estiver pendente e, se a GPU mudou a VRAM
    // (gpuDirty), copia os 4 MB de volta para a RAM (host). E caro: o log
    // "readbacks" do resumo conta quantas vezes aconteceu.
    void download() const {
        flushBatch();flushImageWrites();
        if(!gpuDirty || !host) return;
        // Overwriting host VRAM must not race CPU fallbacks still queued for
        // parallel bands (normally none: GPU draws upload, which drains).
        cpu.Flush();
        D3D11_MAPPED_SUBRESOURCE map{};
        {
            Span span(timed?&timing.wait:nullptr);
            context->CopyResource(staging.Get(),memory.Get());
            checked(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map),"Read GPU GS memory");
        }
        { Span span(timed?&timing.copy:nullptr);std::memcpy(host,map.pData,vramBytes); }
        context->Unmap(staging.Get(),0);
        gpuDirty=false;++stats.readbacks;++timing.readbacks;
        cpu.TextureFlush();
    }
    void clearCluts() {
        // Slot 0 stays all zero: the palette after Initialize/Reset.
        const UINT zero[4]={0,0,0,0};context->ClearUnorderedAccessViewUint(clutUav.Get(),zero);
        clutSlot=0;cpuClutSlot=0;batchFirstSlot=0;
    }
    std::array<uint16_t,512> readClut(uint32_t slot) const {
        const D3D11_BOX box{slot*1024,0,0,slot*1024+1024,1,1};
        context->CopySubresourceRegion(clutStaging.Get(),0,0,0,0,clutBuffer.Get(),0,&box);
        D3D11_MAPPED_SUBRESOURCE map{};
        checked(context->Map(clutStaging.Get(),0,D3D11_MAP_READ,0,&map),"Read GPU CLUT");
        std::array<uint16_t,512> clut{};std::memcpy(clut.data(),map.pData,1024);context->Unmap(clutStaging.Get(),0);
        return clut;
    }
    void syncCpuClut() {
        if(cpuClutSlot==clutSlot) return;
        cpu.SetClutTable(readClut(clutSlot));cpuClutSlot=clutSlot;
    }
    // OROCHI_GPU_VERIFY: the CPU result for a primitive, from current VRAM and CLUT.
    std::vector<uint8_t> referenceDraw(const GSPrimitiveBatch &batch) {
        flushBatch();download();
        std::vector<uint8_t> reference(host,host+vramBytes);
        if(!verifier) verifier=std::make_unique<GSCpuBackend>();
        verifier->Initialize(reference.data(),vramBytes);verifier->SetClutTable(readClut(clutSlot));
        verifier->Submit(batch);verifier->Sync(GSSyncReason::Finish);
        return reference;
    }
    void compareReference(const GSPrimitiveBatch &batch,const std::vector<uint8_t> &reference) {
        flushBatch();download();++stats.verified;--verifyRemaining;
        if(std::memcmp(reference.data(),host,vramBytes)!=0) {
            size_t first=0,count=0;
            for(size_t i=0;i<vramBytes;++i) if(reference[i]!=host[i]) { if(!count) first=i; ++count; }
            const auto &s=batch.state;const auto &c=s.context;
            std::cout << "[Orochi:gpu-verify] MISMATCH primitive=" << stats.verified << " type=" << unsigned(s.prim.type)
                << " bytes=" << count << " first=0x" << std::hex << first << std::dec
                << " gpu=" << unsigned(host[first]) << " cpu=" << unsigned(reference[first])
                << " tme=" << s.prim.tme << " tpsm=" << unsigned(c.tex0.psm) << " fst=" << s.prim.fst
                << " lin=" << s.linearFilter << " iip=" << s.prim.iip << " abe=" << s.prim.abe << " fge=" << s.prim.fge
                << " test=0x" << std::hex << c.test << " alpha=0x" << c.alpha << std::dec
                << " zpsm=" << unsigned(c.zbuf.psm) << " fpsm=" << unsigned(c.frame.psm) << '\n';
            ++stats.mismatches;
            // Continue from the CPU result so later comparisons stay independent.
            std::memcpy(host,reference.data(),vramBytes);cpuDirty=true;
        }
        if(stats.verified==1 || !verifyRemaining || stats.verified%1000==0)
            std::cout << "[Orochi:gpu-verify] checked=" << stats.verified << " mismatches=" << stats.mismatches << '\n';
    }
    mutable std::mutex mutex;
    mutable GSCpuBackend cpu;
    std::unique_ptr<GSCpuBackend> verifier;
    uint8_t *host{};
    mutable bool gpuDirty{};
    mutable bool cpuDirty=true;
    mutable GpuStats stats;
    unsigned long verifyRemaining{};
    uint64_t verifySkip{};
    bool profileFallbacks{},timed{};
    mutable GpuTiming timing;
    mutable unsigned rejectLine{};
    std::map<std::string,uint64_t> fallbackAreas,fallbacksReported;
    bool nativeTransfer{},rawTransfer{};
    uint64_t transferId{};
    RawRange transferRange{};
    mutable std::vector<uint8_t> rawBytes;
    mutable std::vector<RawSegment> rawSegments;
    mutable std::vector<uint32_t> rawGroups;
    mutable std::vector<RawRange> rawRanges;
    GSTransferCommand transfer{};
    GSTransferSnapshot transferState{};
    uint32_t transferPixels{};
    std::array<uint32_t,2> clutCbp{};
    uint32_t clutSlot{},cpuClutSlot{},batchFirstSlot{};
    mutable std::vector<GpuPrim> prims;
    mutable std::vector<GpuDrawState> states;
    mutable std::vector<std::vector<uint32_t>> bins;
    mutable std::vector<uint32_t> touched,indexList;
    mutable std::vector<std::array<uint32_t,4>> tileList;
    mutable std::vector<ByteRange> queueWrites,queueReads;
    mutable uint32_t tileRefs{};
    mutable std::vector<std::array<uint32_t,4>> pendingImage;    // word address, value, mask
    mutable std::vector<uint32_t> pendingImageWords;               // entry index + 1 per word
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ComputeShader> drawShader,imageShader,clutShader,rawShader;
    ComPtr<ID3D11Buffer> memory,staging,clutBuffer,clutStaging,stateBuffer,primBuffer,tileBuffer,indexBuffer,
        imageBuffer,imageConstants,clutConstants,rawBuffer,rawSegmentBuffer,rawGroupBuffer,layoutBuffer;
    ComPtr<ID3D11UnorderedAccessView> uav,clutUav;
    ComPtr<ID3D11ShaderResourceView> stateSrv,primSrv,tileSrv,indexSrv,imageSrv,rawSrv,rawSegmentSrv,rawGroupSrv,layoutSrv;
    ComPtr<ID3D11Query> finishQuery;
};
}

// createOrochiGpuBackend: cria o renderizador; em caso de erro devolve
// nullptr e o motivo em "error" (main.cpp mostra e usa a CPU).
std::unique_ptr<GSRasterBackend> createOrochiGpuBackend(std::string &error) {
    try { return std::make_unique<OrochiGpuBackend>(); }
    catch(const std::exception &problem) { error=problem.what();return {}; }
}

// runGpuRegressionTests (--test-gpu): desenha centenas de casos (testes de
// alfa, profundidade, blend, texturas, paletas, envio de imagens) na GPU
// e na CPU e exige VRAM identica. Precisa de uma GPU real.
int runGpuRegressionTests() {
    std::string error;auto backend=createOrochiGpuBackend(error);
    if(!backend) {std::cerr << "[GPU regression] hardware initialization failed: " << error << '\n';return 2;}
    auto *gpu=static_cast<OrochiGpuBackend *>(backend.get());
    std::vector<uint8_t> actual(vramBytes),expected(vramBytes);
    for(size_t i=0;i<actual.size();++i) actual[i]=expected[i]=uint8_t((i*37+(i>>9))&255);
    GSCpuBackend reference;reference.Initialize(expected.data(),vramBytes);gpu->Initialize(actual.data(),vramBytes);
    GSPrimitiveBatch batch{};batch.vertexCount=2;batch.state.prim.type=GS_PRIM_SPRITE;
    auto &c=batch.state.context;c.frame={4,8,GS_PSM_CT32,0};c.zbuf={256,GS_PSM_Z32,true};
    c.scissor={3,120,5,110};c.test=0x30000;
    batch.vertices[0].x=0;batch.vertices[0].y=0;auto &v=batch.vertices[1];v.x=128;v.y=112;v.z=0x123456;
    v.r=213;v.g=109;v.b=47;v.a=64;
    unsigned cases=0;
    auto compare=[&](const char *label) {
        // The CPU reference may queue primitives for parallel bands.
        reference.Sync(GSSyncReason::Finish);
        std::vector<uint8_t> result;gpu->SnapshotVram(result);++cases;
        if(result!=expected) {auto first=std::mismatch(result.begin(),result.end(),expected.begin()).first;
            std::cerr << "[GPU regression] FAIL " << label << " case=" << cases << " offset=" << std::distance(result.begin(),first) << '\n';return false;}
        return true;
    };
    auto check=[&](const char *label) {
        reference.Submit(batch);gpu->Submit(batch);return compare(label);
    };
    if(!check("scissor and full VRAM guards"))return 1;
    for(unsigned atst=0;atst<8;++atst)for(unsigned afail=0;afail<4;++afail) {
        c.test=0x30001|(atst<<1)|(64<<4)|(afail<<12);
        if(!check("alpha test/failure"))return 1;
    }
    c.test=0x30000;batch.state.prim.abe=true;
    for(unsigned mode=0;mode<256;++mode) {
        c.alpha=uint64_t(mode)|(uint64_t(97)<<32);
        if(!check("all alpha selectors"))return 1;
    }
    c.frame.fbmsk=0xff00ff00;c.fba=1;c.test=0x3c000;
    if(!check("destination alpha, framebuffer mask, FBA"))return 1;
    c.frame.psm=GS_PSM_CT24;c.test=0x30000;
    if(!check("RGB24 preserves upper byte"))return 1;
    c.frame.psm=GS_PSM_CT32;c.frame.fbmsk=0;c.fba=0;
    batch.state.pabe=true;batch.state.prim.fge=true;batch.state.fogR=36;batch.state.fogG=92;batch.state.fogB=121;v.fog=134;
    if(!check("PABE and fog"))return 1;
    batch.state.prim.abe=false;batch.state.prim.fge=false;c.zbuf.zmask=false;
    for(unsigned z=1;z<4;++z) {c.test=(z<<17);if(!check("depth compare/write"))return 1;}
    c.zbuf.psm=GS_PSM_Z24;if(!check("depth24 preserves upper byte"))return 1;
    for(const auto format:{GS_PSM_Z16,GS_PSM_Z16S})for(unsigned masked=0;masked<2;++masked)
    for(unsigned method=1;method<4;++method)for(const unsigned value:{0u,0x1234u,0xffffu}) {
        c.zbuf.psm=format;c.zbuf.zmask=masked;c.test=method<<17;v.z=value;
        if(!check("Z16/Z16S comparison, masked depth and atomic halfword writes"))return 1;
    }
    batch.state.prim.tme=true;batch.state.prim.fst=true;batch.state.pabe=false;
    c.tex0={4096,1,GS_PSM_CT32,6,6,1,1};batch.state.textureWidth=64;batch.state.textureHeight=64;
    batch.vertices[0].u=7;batch.vertices[0].v=16;
    v.u=64*16+11;v.v=63*16+9;c.zbuf.zmask=true;c.test=0x30000;
    for(unsigned filter=0;filter<2;++filter) for(unsigned tfx=0;tfx<4;++tfx)
    for(unsigned tcc=0;tcc<2;++tcc) for(unsigned wrap=0;wrap<4;++wrap) {
        batch.state.linearFilter=filter;c.tex0.tfx=tfx;c.tex0.tcc=tcc;
        c.clamp=wrap|(wrap<<2)|(7ull<<4)|(31ull<<14)|(3ull<<24)|(27ull<<34);
        if(!check("RGBA32 texture/filter/TFX/TCC/wrap"))return 1;
    }
    batch.vertices[0].u=65535;v.u=0;c.tex0.tfx=0;c.tex0.tcc=1;
    for(unsigned atst=0;atst<8;++atst)for(unsigned afail=0;afail<4;++afail) {
        c.test=0x30001|(atst<<1)|(64<<4)|(afail<<12);
        if(!check("flipped texture and varying source alpha"))return 1;
    }
    c.test=0x30000;c.tex0.psm=GS_PSM_CT24;
    if(!check("RGB24 texture"))return 1;
    c.tex0.psm=GS_PSM_CT32;
    if(!check("GPU texture resumes"))return 1;
    c.tex0.tbp0=c.frame.fbp*32;
    if(!check("texture/framebuffer alias falls back to the CPU"))return 1;
    c.tex0.tbp0=4096;batch.state.prim.fst=false;
    batch.vertices[0].s=-0.125f;batch.vertices[0].t=0.05f;batch.vertices[0].q=0.5f;
    v.s=0.9f;v.t=1.2f;v.q=0.75f;
    for(unsigned filter=0;filter<2;++filter)for(unsigned wrap=0;wrap<4;++wrap) {
        batch.state.linearFilter=filter;c.clamp=wrap|(wrap<<2)|(7ull<<4)|(31ull<<14)|(3ull<<24)|(27ull<<34);
        if(!check("ST/Q texture coordinates and negative wrap"))return 1;
    }
    batch.state.prim.fst=true;batch.state.textureWidth=512;batch.state.textureHeight=256;
    c.tex0.tbw=8;c.tex0.tw=9;c.tex0.th=8;c.clamp=5;
    c.scissor={0,511,0,447};v.x=512;v.y=224;
    batch.vertices[0].u=0;batch.vertices[0].v=0;v.u=512*16;v.v=224*16;
    if(!check("full-width menu texture across GS pages"))return 1;
    batch.state.prim.tme=false;v.r=27;
    if(!check("GPU resumes after fallback"))return 1;
    {
        // Screen effects seen in battle (a type ability's glow): bilinear,
        // blended copies of the frame (block 0) into a work buffer 64 pages
        // later, and a full-screen pass under a 2048-row scissor. Both stay
        // on the GPU and must match the CPU.
        GSPrimitiveBatch effect{};effect.vertexCount=2;effect.state.prim.type=GS_PRIM_SPRITE;
        auto &e=effect.state.context;auto &s=effect.state;
        s.prim.tme=true;s.prim.fst=true;s.prim.abe=true;s.linearFilter=true;
        e.frame={64,8,GS_PSM_CT32,0};e.zbuf={120,GS_PSM_Z32,true};e.test=0x33001;e.alpha=0x8000000054ull;
        e.tex0={0,8,GS_PSM_CT32,9,8,1,0};e.clamp=5;s.textureWidth=512;s.textureHeight=256;
        e.xyoffset={uint16_t(1792<<4),uint16_t(1936<<4)};e.scissor={0,63,0,223};
        effect.vertices[0].x=1792;effect.vertices[0].y=1936;effect.vertices[0].u=8*16;effect.vertices[0].v=8*16;
        effect.vertices[1].x=2303;effect.vertices[1].y=2159.5f;effect.vertices[1].u=504*16;effect.vertices[1].v=216*16;
        effect.vertices[1].r=effect.vertices[1].g=effect.vertices[1].b=128;effect.vertices[1].a=120;
        const auto fallbacksBefore=gpu->counters().fallbacks;
        for(unsigned pass=0;pass<4;++pass) {
            e.scissor={uint16_t(pass*64),uint16_t(pass*64+63),0,223};
            reference.Submit(effect);gpu->Submit(effect);
        }
        if(!compare("screen-copy sprites into the pages after their source"))return 1;
        // The full-screen pass samples one texel pair inside the depth buffer
        // (block 2048 + row 448 = page 176) and writes frame 0 or frame 140.
        e.frame={0,10,GS_PSM_CT32,0};e.zbuf={176,GS_PSM_Z32,true};e.test=0x30000;e.alpha=0x8000000044ull;
        e.tex0={2048,8,GS_PSM_CT32,9,9,1,0};e.clamp=0;s.textureHeight=512;s.prim.abe=false;
        e.xyoffset={0,0};e.scissor={0,2047,0,2047};
        effect.vertices[0].x=0;effect.vertices[0].y=0;effect.vertices[1].x=640;effect.vertices[1].y=448;
        effect.vertices[0].u=effect.vertices[1].u=8200;effect.vertices[0].v=effect.vertices[1].v=7176;
        reference.Submit(effect);gpu->Submit(effect);
        if(!compare("full-screen sprite under a 2048-row scissor"))return 1;
        if(gpu->counters().fallbacks!=fallbacksBefore) {
            std::cerr << "[GPU regression] FAIL screen-effect sprites fell back to the CPU\n";return 1;
        }
        // Frame 140 covers page 176: the pass overwrites its own texels while
        // drawing, so only the CPU's pixel order gives the GS result.
        e.frame={140,10,GS_PSM_CT32,0};
        reference.Submit(effect);gpu->Submit(effect);
        if(!compare("full-screen sprite that overwrites its own texels")||gpu->counters().fallbacks!=fallbacksBefore+1) {
            std::cerr << "[GPU regression] FAIL self-sampling sprite must fall back\n";return 1;
        }
    }
    auto stateMatches=[&]() {
        const auto a=reference.GetTransferSnapshot(),b=gpu->GetTransferSnapshot();
        const bool same=a.x==b.x && a.y==b.y && a.totalPixels==b.totalPixels && a.copiedPixels==b.copiedPixels &&
            a.direction==b.direction && a.localToHostPendingBytes==b.localToHostPendingBytes;
        if(!same)
            std::cerr << "[GPU regression] transfer state cpu/gpu x=" << a.x << '/' << b.x << " y=" << a.y << '/' << b.y
                << " total=" << a.totalPixels << '/' << b.totalPixels << " copied=" << a.copiedPixels << '/' << b.copiedPixels
                << " direction=" << unsigned(a.direction) << '/' << unsigned(b.direction) << std::endl;
        return same;
    };
    GSTransferCommand image{};image.direction=0;image.bitbltbuf.dbp=4096;image.bitbltbuf.dbw=8;
    image.bitbltbuf.dpsm=GS_PSM_CT32;image.trxpos.dsax=7;image.trxpos.dsay=15;image.trxreg={127,33};
    std::vector<uint8_t> payload(512*224*4+32);
    for(size_t i=0;i<payload.size();++i) payload[i]=uint8_t(i*19+(i>>7));
    reference.BeginTransfer(image);gpu->BeginTransfer(image);
    unsigned consumed=0;
    for(const unsigned bytes:{4u,128u,7u,127u*33u*4u}) {
        reference.UploadImage(payload.data()+consumed,bytes);gpu->UploadImage(payload.data()+consumed,bytes);consumed+=bytes;
        if(!stateMatches() || !compare("native IMAGE fragments, progress, padding and guards"))return 1;
    }
    image.trxpos.dsax=image.trxpos.dsay=0;image.trxreg={512,224};
    reference.BeginTransfer(image);gpu->BeginTransfer(image);
    reference.UploadImage(payload.data(),512*224*4);gpu->UploadImage(payload.data(),512*224*4);
    if(!stateMatches() || !compare("native IMAGE larger than one GPU input buffer"))return 1;
    image.trxreg={16,16};
    reference.BeginTransfer(image);gpu->BeginTransfer(image);
    reference.UploadImage(payload.data(),16*16*4);gpu->UploadImage(payload.data(),16*16*4);
    image.trxpos.dsax=32;
    reference.BeginTransfer(image);gpu->BeginTransfer(image);
    reference.UploadImage(payload.data()+1024,16*16*4);gpu->UploadImage(payload.data()+1024,16*16*4);
    image.trxpos.dsax=0;
    reference.BeginTransfer(image);gpu->BeginTransfer(image);
    reference.UploadImage(payload.data()+2048,16*16*4);gpu->UploadImage(payload.data()+2048,16*16*4);
    if(!stateMatches() || !compare("batched disjoint images and overlapping replacement preserve order"))return 1;
    // Every host-to-local format, odd rectangles, chunks that split pixels.
    for(const GSPsm format:{GS_PSM_CT24,GS_PSM_Z24,GS_PSM_Z32,GS_PSM_CT16,GS_PSM_CT16S,GS_PSM_Z16,GS_PSM_Z16S,
                            GS_PSM_T8,GS_PSM_T8H,GS_PSM_T4,GS_PSM_T4HL,GS_PSM_T4HH})
    for(unsigned round=0;round<3;++round) {
        image.bitbltbuf.dpsm=format;image.bitbltbuf.dbp=4096+round*512;image.bitbltbuf.dbw=uint8_t(1+round*3);
        image.trxpos.dsax=uint16_t(round*5);image.trxpos.dsay=uint16_t(round*3);image.trxreg={uint16_t(17+round*20),uint16_t(9+round*7)};
        reference.BeginTransfer(image);gpu->BeginTransfer(image);
        const uint32_t bytes=uint32_t(image.trxreg.rrw)*image.trxreg.rrh*4u;
        for(uint32_t sent=0;sent<bytes;) {
            const uint32_t chunk=std::min<uint32_t>(bytes-sent,7u+round*13u);
            reference.UploadImage(payload.data()+sent,chunk);gpu->UploadImage(payload.data()+sent,chunk);sent+=chunk;
        }
        if(!stateMatches() || !compare("native IMAGE in every format, partial pixels and wrapping"))return 1;
    }
    // 128-pixel pages on a 64-pixel buffer: page rows share pages, so this
    // one keeps the per-word path (raw upload needs distinct bits per pixel).
    image.bitbltbuf.dpsm=GS_PSM_T8;image.bitbltbuf.dbp=4096;image.bitbltbuf.dbw=1;
    image.trxpos.dsax=3;image.trxpos.dsay=58;image.trxreg={40,12};
    reference.BeginTransfer(image);gpu->BeginTransfer(image);
    reference.UploadImage(payload.data(),40*12);gpu->UploadImage(payload.data(),40*12);
    if(!stateMatches() || !compare("IMAGE whose page rows share pages"))return 1;
    image.trxreg={31,9};image.bitbltbuf.dpsm=GS_PSM_CT24;image.bitbltbuf.dbp=4096;image.bitbltbuf.dbw=8;image.trxpos.dsax=image.trxpos.dsay=0;
    image.direction=2;image.bitbltbuf.sbp=4096;image.bitbltbuf.sbw=8;image.bitbltbuf.spsm=GS_PSM_CT32;
    reference.BeginTransfer(image);gpu->BeginTransfer(image);
    if(!stateMatches() || !compare("local-to-local transfer falls back and preserves native GPU writes"))return 1;
    image.direction=0;
    reference.Submit(batch);gpu->Submit(batch);
    const auto readbacksBeforeFinish=gpu->counters().readbacks;
    gpu->Sync(GSSyncReason::Finish);
    if(gpu->counters().readbacks!=readbacksBeforeFinish || !compare("FINISH fences GPU writes without a full-VRAM readback"))return 1;
    batch.state.prim.tme=true;c.tex0={4096,1,GS_PSM_T4,6,6,1,1};
    batch.state.textureWidth=batch.state.textureHeight=64;c.tex0.cpsm=GS_PSM_CT32;c.tex0.cbp=6000;c.tex0.cld=2;
    GSTexClutReg palette{};
    reference.LoadClut(c.tex0,palette);gpu->LoadClut(c.tex0,palette);
    if(!check("indexed texture uses the loaded CLUT bank"))return 1;
    c.tex0.cld=4;
    reference.WriteVram(GS_PSM_CT32,6000,1,0,0,0x80765432);
    gpu->WriteVram(GS_PSM_CT32,6000,1,0,0,0x80765432);
    reference.LoadClut(c.tex0,palette);gpu->LoadClut(c.tex0,palette);
    if(!check("unchanged conditional CLUT bank retains old palette"))return 1;
    c.tex0.cbp=6001;reference.LoadClut(c.tex0,palette);gpu->LoadClut(c.tex0,palette);
    if(!check("changed conditional CLUT bank loads the new palette"))return 1;
    // Randomized triangles and sprites over every format the GPU path takes,
    // queued in batches (compared once per batch) against the CPU rasterizer.
    // Textures and palettes sit apart from the frame and depth buffers.
    std::mt19937 random(1234);
    auto pick=[&](uint32_t n) { return uint32_t(random()%n); };
    auto real=[&](float low,float high) { return std::uniform_real_distribution<float>(low,high)(random); };
    const GSPsm textures[]={GS_PSM_CT32,GS_PSM_CT24,GS_PSM_CT16,GS_PSM_CT16S,GS_PSM_T8,GS_PSM_T4,GS_PSM_T8H,GS_PSM_T4HL,GS_PSM_T4HH};
    const GSPsm palettes[]={GS_PSM_CT32,GS_PSM_CT24,GS_PSM_CT16,GS_PSM_CT16S};
    const GSPsm depths[]={GS_PSM_Z32,GS_PSM_Z24,GS_PSM_Z16,GS_PSM_Z16S};
    for(unsigned round=0;round<300;++round) {
        // One frame/depth layout per round, so the GPU queues long batches.
        const GSPsm framePsm=pick(4)==0?GS_PSM_CT24:GS_PSM_CT32,depthPsm=depths[pick(4)];
        for(unsigned n=0;n<64;++n) {
            GSPrimitiveBatch b{};auto &s=b.state;auto &k=s.context;
            const bool sprite=pick(4)==0;
            s.prim.type=sprite?GS_PRIM_SPRITE:GSPrimType(GS_PRIM_TRIANGLE+pick(3));b.vertexCount=sprite?2:3;
            s.prim.iip=pick(2);s.prim.tme=pick(4)!=0;s.prim.fst=pick(2);s.prim.abe=pick(2);s.prim.fge=pick(2);
            s.pabe=pick(4)==0;s.linearFilter=pick(2);s.fogR=uint8_t(pick(256));s.fogG=uint8_t(pick(256));s.fogB=uint8_t(pick(256));
            k.frame={4,8,framePsm,pick(6)==0?0x00ff00ffu:0u};
            k.zbuf={120,depthPsm,pick(3)==0};k.scissor={uint16_t(pick(40)),uint16_t(400+pick(100)),uint16_t(pick(40)),uint16_t(300+pick(100))};
            k.xyoffset={uint16_t(pick(64)<<4),uint16_t(pick(64)<<4)};
            k.test=(pick(2)) | (pick(8)<<1) | (pick(256)<<4) | (pick(4)<<12) | (pick(6)==0?(1u<<14)|(pick(2)<<15):0u) |
                   (1u<<16) | (pick(4)<<17);
            k.alpha=pick(256)|(uint64_t(pick(256))<<32);k.fba=pick(4)==0;
            k.tex0={uint32_t(8192+pick(8)*256),uint8_t(1+pick(2)),textures[pick(9)],6,6,uint8_t(pick(2)),uint8_t(pick(4)),
                    uint32_t(7600+pick(16)*4),palettes[pick(4)],0,uint8_t(pick(16)),0};
            s.textureWidth=uint16_t(16u<<pick(3));s.textureHeight=uint16_t(16u<<pick(3));
            k.clamp=pick(4)|(pick(4)<<2)|(uint64_t(pick(32))<<4)|(uint64_t(pick(64))<<14)|(uint64_t(pick(32))<<24)|(uint64_t(pick(64))<<34);
            s.texa={uint8_t(pick(256)),bool(pick(2)),uint8_t(pick(256))};
            for(unsigned i=0;i<b.vertexCount;++i) {
                auto &x=b.vertices[i];
                x.x=real(0,560)+float(k.xyoffset.ofx>>4);x.y=real(0,420)+float(k.xyoffset.ofy>>4);
                x.z=pick(3)==0 ? double(pick(65536)) : double(random());
                x.r=uint8_t(pick(256));x.g=uint8_t(pick(256));x.b=uint8_t(pick(256));x.a=uint8_t(pick(256));
                x.s=real(-1.5f,2.5f);x.t=real(-1.5f,2.5f);x.q=pick(8)==0?0.0f:real(0.2f,3.0f);
                x.u=uint16_t(pick(4096));x.v=uint16_t(pick(4096));x.fog=uint8_t(pick(256));
            }
            if(isIndexed(k.tex0.psm) && pick(3)==0) {
                k.tex0.cld=1;k.tex0.csm=0;reference.LoadClut(k.tex0,palette);gpu->LoadClut(k.tex0,palette);
            }
            reference.Submit(b);gpu->Submit(b);
        }
        if(!compare("randomized triangles, sprites, textures, CLUTs and depth formats"))return 1;
    }
    const auto stats=gpu->counters();
    std::cout << "[GPU regression] " << cases << " complete-VRAM comparisons PASS; triangles=" << stats.triangles
        << " sprites=" << stats.sprites << " batches=" << stats.batches << " clutLoads=" << stats.clutLoads
        << " fallbacks=" << stats.fallbacks << " nativeImagePixels=" << stats.imagePixels << '\n';
    return stats.triangles>0&&stats.sprites>0&&stats.fallbacks>0&&stats.imagePixels>0&&stats.clutLoads>0?0:1;
}
#else
std::unique_ptr<GSRasterBackend> createOrochiGpuBackend(std::string &error) {error="D3D11 requires Windows";return {};}
int runGpuRegressionTests() {return 2;}
#endif
