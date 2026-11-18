#pragma once
// GS rasterization on the GPU, bit-exact with GSCpuBackend. The host only
// queues primitives whose formats this shader implements and that do not
// sample or alias what they write; it bins them into 16x16 pixel tiles. One
// thread group owns a tile and walks its primitives in submission order, so
// every pixel sees the same sequence as on the GS.
//
// Floating-point steps mirror the CPU code operation by operation (`precise`
// blocks reassociation and fused multiply-add). Divisions are corrected to
// IEEE rounding with exact double-precision residuals because D3D11 float
// division is approximate.
// Split into several literals: MSVC limits each piece to 16 KB.
constexpr char orochiGpuRasterShader[] = R"hlsl(
struct DrawState {
    uint4 frame;   // block base, buffer width, format, write mask
    uint4 zbuf;    // block base, format, write disabled, TEST
    uint4 blend;   // ALPHA selectors | FIX<<8, flags, CLUT slot, fog RGB
    uint4 tex;     // block base, buffer width, format, TFX | TCC<<2 | CPSM<<8 | CSM<<16 | CSA<<24
    uint4 tex2;    // width, height, TA0 | TA1<<8 | AEM<<16, wrap U | wrap V<<2
    uint4 region;  // MINU, MAXU, MINV, MAXV
};
// blend.y flags
static const uint F_ABE=1, F_PABE=2, F_FBA=4, F_FGE=8, F_TME=16, F_FST=32, F_IIP=64, F_LINEAR=128;
struct Prim {
    uint4 head;    // kind (0 triangle, 1 sprite), state, x0 | y0<<16, x1 | y1<<16 (inclusive)
    float4 a;      // triangle: y1-y2, x2-x1, y2-y0, x0-x2   sprite: u0, v0, u1, v1
    float4 b;      // triangle: x2, y2, winding, 1/|area|   sprite: origin x, origin y, span x, span y (ints)
    uint4 v0, v1, v2;     // Z low, Z high, RGBA, U | V<<16 (sprite: Z, -, RGBA, -)
    float4 w0, w1, w2;    // S, T, Q, fog
};
RWByteAddressBuffer memory : register(u0);
RWByteAddressBuffer cluts : register(u1);
cbuffer ImageUpload : register(b1) {
    uint4 imageTarget;
    uint4 imageRegion;          // submitted pixel count in z
};
cbuffer ClutLoad : register(b2) {
    uint4 clutA;                // source slot, destination slot, CBP, CPSM
    uint4 clutB;                // CSM, CSA, 4-bit source, CBW
    uint4 clutC;                // COU, COV
};
Buffer<uint4> imagePixels : register(t1);  // word address, value, mask of bits written
StructuredBuffer<DrawState> states : register(t2);
StructuredBuffer<Prim> prims : register(t3);
StructuredBuffer<uint4> tiles : register(t4);  // tile x, tile y, first index, count
StructuredBuffer<uint> tileIndices : register(t5);
struct RawSegment {
    uint4 a;   // DBP, DBW, DSAX, DSAY
    uint4 b;   // RRW, first pixel, transfer pixels, first byte
    uint4 c;   // bytes per unit, pixels per unit, units, first thread group
    uint4 d;   // page W | H<<16, column W | H<<16, block table | column table<<16,
               // unpacked bits | bit offset<<8 | packed bytes<<16 | written bits<<24
};
StructuredBuffer<RawSegment> rawSegments : register(t6);
Buffer<uint> rawGroups : register(t7);     // segment of each thread group
Buffer<uint> rawWords : register(t8);      // the IMAGE bytes
Buffer<uint> layoutTables : register(t9);  // block and column tables (layout::tables)

static const uint blocks32[32] = {
 0,1,4,5,16,17,20,21, 2,3,6,7,18,19,22,23,
 8,9,12,13,24,25,28,29, 10,11,14,15,26,27,30,31
};
static const uint zblocks16[32]={
 24,26,16,18,25,27,17,19,28,30,20,22,29,31,21,23,
 8,10,0,2,9,11,1,3,12,14,4,6,13,15,5,7
};
static const uint zblocks16s[32]={
 24,26,8,10,25,27,9,11,16,18,0,2,17,19,1,3,
 28,30,12,14,29,31,13,15,20,22,4,6,21,23,5,7
};
static const uint columns16[32]={
 0,2,8,10,16,18,24,26,1,3,9,11,17,19,25,27,
 4,6,12,14,20,22,28,30,5,7,13,15,21,23,29,31
};
static const uint blocksC16[32]={
 0,2,8,10, 1,3,9,11, 4,6,12,14, 5,7,13,15, 16,18,24,26, 17,19,25,27, 20,22,28,30, 21,23,29,31
};
static const uint blocksC16S[32]={
 0,2,16,18, 1,3,17,19, 8,10,24,26, 9,11,25,27, 4,6,20,22, 5,7,21,23, 12,14,28,30, 13,15,29,31
};
static const uint blocksP4[32]={
 0,2,8,10, 1,3,9,11, 4,6,12,14, 5,7,13,15, 16,18,24,26, 17,19,25,27, 20,22,28,30, 21,23,29,31
};
static const uint columnTable16[128]={
  0,  2,  8, 10, 16, 18, 24, 26,  1,  3,  9, 11, 17, 19, 25, 27,
  4,  6, 12, 14, 20, 22, 28, 30,  5,  7, 13, 15, 21, 23, 29, 31,
 32, 34, 40, 42, 48, 50, 56, 58, 33, 35, 41, 43, 49, 51, 57, 59,
 36, 38, 44, 46, 52, 54, 60, 62, 37, 39, 45, 47, 53, 55, 61, 63,
 64, 66, 72, 74, 80, 82, 88, 90, 65, 67, 73, 75, 81, 83, 89, 91,
 68, 70, 76, 78, 84, 86, 92, 94, 69, 71, 77, 79, 85, 87, 93, 95,
 96, 98,104,106,112,114,120,122, 97, 99,105,107,113,115,121,123,
100,102,108,110,116,118,124,126,101,103,109,111,117,119,125,127
};
static const uint columnTable8[256]={
   0,  4, 16, 20, 32, 36, 48, 52,  2,  6, 18, 22, 34, 38, 50, 54,
   8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62,
  33, 37, 49, 53,  1,  5, 17, 21, 35, 39, 51, 55,  3,  7, 19, 23,
  41, 45, 57, 61,  9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31,
  96,100,112,116, 64, 68, 80, 84, 98,102,114,118, 66, 70, 82, 86,
 104,108,120,124, 72, 76, 88, 92,106,110,122,126, 74, 78, 90, 94,
  65, 69, 81, 85, 97,101,113,117, 67, 71, 83, 87, 99,103,115,119,
  73, 77, 89, 93,105,109,121,125, 75, 79, 91, 95,107,111,123,127,
 128,132,144,148,160,164,176,180,130,134,146,150,162,166,178,182,
 136,140,152,156,168,172,184,188,138,142,154,158,170,174,186,190,
 161,165,177,181,129,133,145,149,163,167,179,183,131,135,147,151,
 169,173,185,189,137,141,153,157,171,175,187,191,139,143,155,159,
 224,228,240,244,192,196,208,212,226,230,242,246,194,198,210,214,
 232,236,248,252,200,204,216,220,234,238,250,254,202,206,218,222,
 193,197,209,213,225,229,241,245,195,199,211,215,227,231,243,247,
 201,205,217,221,233,237,249,253,203,207,219,223,235,239,251,255
};
static const uint columnTable4[512]={
   0,  8, 32, 40, 64, 72, 96,104,  2, 10, 34, 42, 66, 74, 98,106,  4, 12, 36, 44, 68, 76,100,108,  6, 14, 38, 46, 70, 78,102,110,
  16, 24, 48, 56, 80, 88,112,120, 18, 26, 50, 58, 82, 90,114,122, 20, 28, 52, 60, 84, 92,116,124, 22, 30, 54, 62, 86, 94,118,126,
  65, 73, 97,105,  1,  9, 33, 41, 67, 75, 99,107,  3, 11, 35, 43, 69, 77,101,109,  5, 13, 37, 45, 71, 79,103,111,  7, 15, 39, 47,
  81, 89,113,121, 17, 25, 49, 57, 83, 91,115,123, 19, 27, 51, 59, 85, 93,117,125, 21, 29, 53, 61, 87, 95,119,127, 23, 31, 55, 63,
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
 401,409,433,441,465,473,497,505,403,411,435,443,467,475,499,507,405,413,437,445,469,477,501,509,407,415,439,447,471,479,503,511
};

)hlsl" R"hlsl(
// ---- GS memory layouts (GSMem::PixelStorageTraits) ----
uint address32(uint base, uint width, uint x, uint y, bool z) {
    uint block = blocks32[((y>>3)&3)*8+((x>>3)&7)];
    if (z) block ^= 24;
    uint column = (x&1) + ((y&1)<<1) + ((x&6)<<1) + ((y&6)<<3);
    return ((base*256) + (((y>>5)*width+(x>>6))*8192) + block*256 + column*4) & 0x3ffffc;
}
uint addressZ16(uint base,uint width,uint x,uint y,uint format) {
    uint index=((y>>3)&7)*4+((x>>4)&3);
    uint block=format==58 ? zblocks16s[index] : zblocks16[index];
    return (base*256+((y>>6)*width+(x>>6))*8192+block*256+
           ((y>>1)&3)*64+columns16[(y&1)*16+(x&15)]*2) & 0x3ffffe;
}
uint address16(uint base, uint width, uint x, uint y, bool swizzled) {
    uint page = base/32 + (y>>6)*width + (x>>6);
    uint index = ((y>>3)&7)*4 + ((x>>4)&3);
    uint block = swizzled ? blocksC16S[index] : blocksC16[index];
    uint pixel = page*4096 + ((base&31) + block)*128 + columnTable16[(y&7)*16 + (x&15)];
    return (pixel*2) & 0x3ffffe;
}
uint load16(uint address) { return (memory.Load(address & 0x3ffffc) >> ((address&2)*8)) & 0xffff; }
uint readT8(uint base, uint width, uint x, uint y) {
    uint page = base/32 + (y>>6)*((width*64)/128) + (x>>7);
    uint pixel = page*8192 + ((base&31) + blocks32[((y>>4)&3)*8 + ((x>>4)&7)])*256 + columnTable8[(y&15)*16 + (x&15)];
    uint address = pixel & 0x3fffff;
    return (memory.Load(address & 0x3ffffc) >> ((address&3)*8)) & 255;
}
uint readT4(uint base, uint width, uint x, uint y) {
    uint page = base/32 + (y>>7)*((width*64)/128) + (x>>7);
    uint pixel = page*16384 + ((base&31) + blocksP4[((y>>4)&7)*4 + ((x>>5)&3)])*512 + columnTable4[(y&15)*32 + (x&31)];
    uint bits = pixel*4;
    uint address = (bits>>3) & 0x3fffff;
    uint value = (memory.Load(address & 0x3ffffc) >> ((address&3)*8)) & 255;
    return (value >> (bits&7)) & 15;
}
// GSMem::ReadTexture for every format the host accepts.
uint readTexel(uint psm, uint base, uint width, uint x, uint y) {
    if (psm==0x13) return readT8(base,width,x,y);
    if (psm==0x14) return readT4(base,width,x,y);
    if (psm==0x02) return load16(address16(base,width,x,y,false));
    if (psm==0x0A) return load16(address16(base,width,x,y,true));
    uint v = memory.Load(address32(base,width,x,y,false));
    if (psm==0x01) return v & 0xffffff;
    if (psm==0x1B) return (v>>24) & 255;
    if (psm==0x24) return (v>>24) & 15;
    if (psm==0x2C) return (v>>28) & 15;
    return v;
}

)hlsl" R"hlsl(
// ---- C++ conversions ----
int toInt(float f) {
    // static_cast<int>(float) on x64 (cvttss2si): out of range and NaN give INT_MIN.
    return (isnan(f) || f >= 2147483648.0f || f < -2147483648.0f) ? int(0x80000000) : int(f);
}
uint clampU8(int v) { return uint(clamp(v,0,255)); }
int roundHalfAway(float x) {
    // std::lround
    float m = abs(x);
    precise float whole = floor(m);
    precise float fraction = m - whole;
    int r = int(whole) + (fraction >= 0.5f ? 1 : 0);
    return x < 0 ? -r : r;
}
uint toU32(double value) {
    // static_cast<uint32_t>(double): truncation through a 64-bit integer.
    if (value >= 4294967296.0) return value < 8589934592.0 ? uint(value - 4294967296.0) : 0xffffffff;
    if (value >= 0) return uint(value);
    return value > -2147483648.0 ? uint(int(value)) : 0x80000000;
}
float stepFloat(float q, bool up) {
    uint bits = asuint(q);
    return asfloat((q > 0) == up ? bits + 1 : bits - 1);
}
float exactDiv(float a, float b) {
    precise float q = a / b;
    if (b == 0 || q == 0 || isinf(q) || isnan(q)) return q;
    double da = a, db = b;
    precise double r = da - double(q) * db;
    [loop] for (uint i = 0; i < 4 && r != 0; ++i) {
        // Move to the neighbour closer to the exact quotient (ties to even).
        bool up = (r > 0) == (b > 0);
        float n = stepFloat(q, up);
        precise double rn = da - double(n) * db;
        double ar = r < 0 ? -r : r, an = rn < 0 ? -rn : rn;
        if (an < ar || (an == ar && (asuint(n) & 1) == 0)) { q = n; r = rn; }
        else break;
    }
    return q;
}

)hlsl" R"hlsl(
// ---- texture sampling (GSCpuBackend::SampleTexture / LookupCLUT) ----
uint unpack5551(uint c) {
    return (((c>>0)&31)<<3) | ((((c>>5)&31)<<3)<<8) | ((((c>>10)&31)<<3)<<16) | (((c>>15)&1)<<7<<24);
}
uint applyTexa(DrawState s, uint psm, uint texel) {
    if (psm == 0) return texel;
    bool rgbZero = (texel & 0xffffff) == 0;
    uint a = texel >> 24;
    uint ta0 = s.tex2.z & 255, ta1 = (s.tex2.z >> 8) & 255;
    bool aem = ((s.tex2.z >> 16) & 1) != 0;
    if (psm == 1) a = (aem && rgbZero) ? 0 : ta0;
    else if (psm == 2 || psm == 10) a = (a & 0x80) != 0 ? ta1 : ((aem && rgbZero) ? 0 : ta0);
    return (texel & 0xffffff) | (a << 24);
}
uint clutEntry(uint slot, uint index) {
    uint word = cluts.Load(slot*1024 + (index & ~1u)*2);
    return (index & 1) != 0 ? (word >> 16) : (word & 0xffff);
}
uint lookupClut(DrawState s, uint index) {
    uint psm = s.tex.z, cpsm = (s.tex.w >> 8) & 63, csm = (s.tex.w >> 16) & 1, csa = (s.tex.w >> 24) & 31;
    bool sixteen = cpsm == 2 || cpsm == 10;
    uint clutBase = (csa & (sixteen ? 0x1fu : 0x0fu)) << 4;
    bool fourBit = psm == 0x14 || psm == 0x24 || psm == 0x2C;
    bool eightBit = psm == 0x13 || psm == 0x1B;
    uint source = fourBit ? (index & 15) : index;
    uint clutIndex = (clutBase + source) & (sixteen ? 0x1ffu : 0xffu);
    if (!sixteen && csm == 0 && eightBit)
        clutIndex = min((source & 0xf0) + clutBase, 240u) + (source & 15);
    uint slot = s.blend.z;
    if (cpsm == 0) return clutEntry(slot, clutIndex) | (clutEntry(slot, clutIndex + 256) << 16);
    if (cpsm == 1) return applyTexa(s, 1, (clutEntry(slot, clutIndex) | (clutEntry(slot, clutIndex + 256) << 16)) & 0xffffff);
    if (sixteen) return applyTexa(s, cpsm, unpack5551(clutEntry(slot, clutIndex)));
    return 0xFFFF00FFu;
}
int wrapCoordinate(int value, int size, uint mode, uint low, uint high) {
    if (mode==0) return int(uint(value) & uint(size-1));
    if (mode==1) return clamp(value,0,size-1);
    if (mode==2) return min(max(value,int(low)),int(high));
    return int((uint(value) & low) | high);
}
uint samplePoint(DrawState s, int u, int v) {
    u = wrapCoordinate(u, int(s.tex2.x), s.tex2.w & 3, s.region.x, s.region.y);
    v = wrapCoordinate(v, int(s.tex2.y), (s.tex2.w >> 2) & 3, s.region.z, s.region.w);
    uint psm = s.tex.z;
    uint raw = readTexel(psm, s.tex.x, s.tex.y, uint(u), uint(v));
    if (psm == 0x13 || psm == 0x14 || psm == 0x1B || psm == 0x24 || psm == 0x2C) return lookupClut(s, raw);
    if (psm == 2 || psm == 10) return applyTexa(s, psm, unpack5551(raw));
    return applyTexa(s, psm, raw);
}
uint lerpChannel(uint c00, uint c10, uint c01, uint c11, float fx, float fy) {
    precise float top = float(c00) + (float(c10) - float(c00)) * fx;
    precise float bottom = float(c01) + (float(c11) - float(c01)) * fx;
    precise float value = top + (bottom - top) * fy;
    return clampU8(roundHalfAway(value));
}
// Texel for unwrapped texel-space coordinates (texUf, texVf).
uint sampleTexture(DrawState s, float texU, float texV) {
    if (!((s.blend.y & F_LINEAR) != 0)) return samplePoint(s, toInt(texU), toInt(texV));
    precise float su = texU - 0.5f;
    precise float sv = texV - 0.5f;
    int u0 = toInt(floor(su)), v0 = toInt(floor(sv));
    precise float fx = su - float(u0);
    precise float fy = sv - float(v0);
    uint c00 = samplePoint(s, u0, v0), c10 = samplePoint(s, u0 + 1, v0);
    uint c01 = samplePoint(s, u0, v0 + 1), c11 = samplePoint(s, u0 + 1, v0 + 1);
    uint result = 0;
    [unroll] for (uint shift = 0; shift < 32; shift += 8)
        result |= lerpChannel((c00>>shift)&255, (c10>>shift)&255, (c01>>shift)&255, (c11>>shift)&255, fx, fy) << shift;
    return result;
}
uint4 combineTexture(DrawState s, uint4 v, uint texel) {
    uint4 t = uint4(texel&255, (texel>>8)&255, (texel>>16)&255, texel>>24);
    uint tfx = s.tex.w & 3;
    bool tcc = ((s.tex.w >> 2) & 1) != 0;
    uint4 o = uint4(t.xyz, tcc ? t.w : v.w);
    if (tfx == 0) {
        o.xyz = min((t.xyz * v.xyz) >> 7, 255u);
        o.w = tcc ? min((t.w * v.w) >> 7, 255u) : v.w;
    } else if (tfx >= 2) {
        o.xyz = min(((t.xyz * v.xyz) >> 7) + v.w, 255u);
        o.w = tcc ? (tfx == 2 ? min(t.w + v.w, 255u) : t.w) : v.w;
    }
    return o;
}

)hlsl" R"hlsl(
// ---- GSCpuBackend::WritePixel ----
void writePixel(DrawState s, uint x, uint y, uint z, uint4 c, uint fog) {
    uint flags = s.blend.y;
    if ((flags & F_FGE) != 0) {
        uint fogRgb = s.blend.w;
        uint3 fc = uint3(fogRgb & 255, (fogRgb >> 8) & 255, (fogRgb >> 16) & 255);
        c.xyz = ((fog * c.xyz) >> 8) + (((255 - fog) * fc) >> 8);
    }
    uint test = s.zbuf.w, fpsm = s.frame.z, zpsm = s.zbuf.y;
    bool writeRgb = true, writeAlpha = true, writeDepth = true;
    if ((test & 1) != 0) {
        uint atst = (test >> 1) & 7, aref = (test >> 4) & 255;
        bool passed = atst == 1 || (atst == 2 && c.w < aref) || (atst == 3 && c.w <= aref) ||
            (atst == 4 && c.w == aref) || (atst == 5 && c.w >= aref) || (atst == 6 && c.w > aref) ||
            (atst == 7 && c.w != aref);
        if (!passed) {
            uint failMode = (test >> 12) & 3;
            if (failMode == 0) return;
            if (failMode == 1) writeDepth = false;
            else if (failMode == 2) { writeRgb = false; writeAlpha = false; }
            else { writeDepth = false; if (fpsm == 0) writeAlpha = false; }
        }
    }
    bool writesFrame = writeRgb || writeAlpha;
    bool preserveDestAlpha = writeRgb && !writeAlpha && fpsm == 0;
    bool date = ((test >> 14) & 1) != 0;
    bool frmw = (date && fpsm == 0) || (writesFrame && (s.frame.w != 0 || ((flags & F_ABE) != 0) || preserveDestAlpha));
    uint target = address32(s.frame.x, s.frame.y, x, y, false);
    uint raw = 0, fbrgba = 0;
    if (frmw) {
        raw = memory.Load(target);
        if (fpsm == 1) raw &= 0xffffff;
        fbrgba = fpsm == 1 ? (raw | 0x80000000) : raw;
    }
    if (date && fpsm == 0 && ((raw >> 31) & 1) != ((test >> 15) & 1)) return;
    uint ztest = (test >> 17) & 3;
    if (ztest == 0) return;
    bool z16 = zpsm == 50 || zpsm == 58;
    uint za = z16 ? addressZ16(s.zbuf.x, s.frame.y, x, y, zpsm) : address32(s.zbuf.x, s.frame.y, x, y, true);
    if (ztest >= 2) {
        uint stored = z16 ? load16(za) : memory.Load(za);
        if (zpsm == 49) stored &= 0xffffff;
        if ((ztest == 2 && z < stored) || (ztest == 3 && z <= stored)) return;
    }
    if (writesFrame) {
        if (((flags & F_ABE) != 0) && !(((flags & F_PABE) != 0) && (c.w & 0x80) == 0)) {
            int3 src = int3(c.xyz), dst = int3(fbrgba & 255, (fbrgba >> 8) & 255, (fbrgba >> 16) & 255);
            uint alpha = s.blend.x;
            uint asel = alpha & 3, bsel = (alpha >> 2) & 3, csel = (alpha >> 4) & 3, dsel = (alpha >> 6) & 3;
            int factor = csel == 0 ? int(c.w) : (csel == 1 ? int(fbrgba >> 24) : int((alpha >> 8) & 255));
            int3 A = asel == 0 ? src : (asel == 1 ? dst : int3(0,0,0));
            int3 B = bsel == 0 ? src : (bsel == 1 ? dst : int3(0,0,0));
            int3 D = dsel == 0 ? src : (dsel == 1 ? dst : int3(0,0,0));
            int3 result = (((A - B) * factor) >> 7) + D;
            c.xyz = uint3(clamp(result, 0, 255));
        }
        if (writeAlpha && ((flags & F_FBA) != 0) && fpsm != 1) c.w |= 0x80;
        uint pixel = c.x | (c.y << 8) | (c.z << 16) | (c.w << 24);
        if (s.frame.w != 0) pixel = (pixel & ~s.frame.w) | (fbrgba & s.frame.w);
        if (preserveDestAlpha) pixel = (pixel & 0xffffff) | (fbrgba & 0xff000000);
        if (fpsm == 1) pixel = (memory.Load(target) & 0xff000000) | (pixel & 0xffffff);
        memory.Store(target, pixel);
    }
    if (writeDepth && s.zbuf.z == 0) {
        if (z16) {
            // Adjacent Z16 pixels share a word and belong to the same tile.
            uint aligned = za & 0x3ffffc, shift = (za & 2) * 8;
            memory.InterlockedAnd(aligned, ~(0xffffu << shift));
            memory.InterlockedOr(aligned, (z & 0xffff) << shift);
        } else if (zpsm == 49) {
            memory.Store(za, (memory.Load(za) & 0xff000000) | (z & 0xffffff));
        } else {
            memory.Store(za, z);
        }
    }
}

)hlsl" R"hlsl(
// ---- primitives ----
uint4 unpackColor(uint c) { return uint4(c & 255, (c >> 8) & 255, (c >> 16) & 255, c >> 24); }
void drawTriangle(Prim p, DrawState s, uint x, uint y) {
    precise float px = float(x) + 0.5f;
    precise float py = float(y) + 0.5f;
    precise float dx = px - p.b.x;
    precise float dy = py - p.b.y;
    precise float w0 = ((p.a.x * dx + p.a.y * dy) * p.b.z) * p.b.w;
    precise float w1 = ((p.a.z * dx + p.a.w * dy) * p.b.z) * p.b.w;
    precise float w2 = (1.0f - w0) - w1;
    if (w0 < -1.0e-4f || w1 < -1.0e-4f || w2 < -1.0e-4f) return;
    precise double z = asdouble(p.v0.x, p.v0.y) * double(w0) + asdouble(p.v1.x, p.v1.y) * double(w1) +
                       asdouble(p.v2.x, p.v2.y) * double(w2);
    uint flags = s.blend.y;
    uint4 c0 = unpackColor(p.v0.z), c1 = unpackColor(p.v1.z), c2 = unpackColor(p.v2.z);
    uint4 color = c2;
    if ((flags & F_IIP) != 0) {
        precise float4 shade = float4(c0) * w0 + float4(c1) * w1 + float4(c2) * w2;
        color = uint4(clampU8(toInt(shade.x)), clampU8(toInt(shade.y)), clampU8(toInt(shade.z)), clampU8(toInt(shade.w)));
    }
    if ((flags & F_TME) != 0) {
        float texU, texV;
        if ((flags & F_FST) != 0) {
            precise float fu = float(p.v0.w & 0xffff) * w0 + float(p.v1.w & 0xffff) * w1 + float(p.v2.w & 0xffff) * w2;
            precise float fv = float(p.v0.w >> 16) * w0 + float(p.v1.w >> 16) * w1 + float(p.v2.w >> 16) * w2;
            texU = float(uint(toInt(fu)) & 0xffff) * 0.0625f;
            texV = float(uint(toInt(fv)) & 0xffff) * 0.0625f;
        } else {
            precise float is = p.w0.x * w0 + p.w1.x * w1 + p.w2.x * w2;
            precise float it = p.w0.y * w0 + p.w1.y * w1 + p.w2.y * w2;
            precise float iq = p.w0.z * w0 + p.w1.z * w1 + p.w2.z * w2;
            float q = abs(iq) > 1.0e-8f ? iq : 1.0f;
            precise float invQ = exactDiv(1.0f, q);
            texU = (is * invQ) * float(s.tex2.x);
            texV = (it * invQ) * float(s.tex2.y);
        }
        color = combineTexture(s, color, sampleTexture(s, texU, texV));
    }
    precise float fog = p.w0.w * w0 + p.w1.w * w1 + p.w2.w * w2;
    writePixel(s, x, y, toU32(z + 0.5), color, clampU8(toInt(fog)));
}
void drawSprite(Prim p, DrawState s, uint x, uint y) {
    uint4 color = unpackColor(p.v0.z);
    uint flags = s.blend.y;
    if ((flags & F_TME) != 0) {
        int originX = asint(p.b.x), originY = asint(p.b.y);
        float spanX = float(asint(p.b.z)), spanY = float(asint(p.b.w));
        precise float ty = exactDiv(float(int(y) - originY) + 0.5f, spanY);
        precise float tx = exactDiv(float(int(x) - originX) + 0.5f, spanX);
        precise float texV = p.a.y + (p.a.w - p.a.y) * ty;
        precise float texU = p.a.x + (p.a.z - p.a.x) * tx;
        float sampleU, sampleV;
        if ((flags & F_FST) != 0) {
            precise float fu = texU * 16.0f + 0.5f;
            precise float fv = texV * 16.0f + 0.5f;
            sampleU = float(clamp(toInt(fu), 0, 0xffff)) * 0.0625f;
            sampleV = float(clamp(toInt(fv), 0, 0xffff)) * 0.0625f;
        } else {
            float width = float(s.tex2.x), height = float(s.tex2.y);
            sampleU = exactDiv(texU, width) * width;
            sampleV = exactDiv(texV, height) * height;
        }
        color = combineTexture(s, color, sampleTexture(s, sampleU, sampleV));
    }
    writePixel(s, x, y, p.v0.x, color, uint(p.w0.w));
}
[numthreads(16,16,1)]
void drawBatch(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID) {
    uint4 tile = tiles[group.x];
    uint x = tile.x * 16 + local.x, y = tile.y * 16 + local.y;
    [loop] for (uint i = 0; i < tile.w; ++i) {
        Prim p = prims[tileIndices[tile.z + i]];
        if (x < (p.head.z & 0xffff) || x > (p.head.w & 0xffff) || y < (p.head.z >> 16) || y > (p.head.w >> 16)) continue;
        DrawState s = states[p.head.y];
        if (p.head.x == 0) drawTriangle(p, s, x, y);
        else drawSprite(p, s, x, y);
    }
}

)hlsl" R"hlsl(
// ---- IMAGE transfers and CLUT loads ----
[numthreads(64,1,1)]
void uploadImage(uint3 thread : SV_DispatchThreadID) {
    if(thread.x>=imageRegion.z) return;
    uint4 write=imagePixels[thread.x];
    // One entry per word and dispatch, so a plain read-modify-write is safe.
    uint value=write.z==0xffffffffu ? write.y : ((memory.Load(write.x)&~write.z)|(write.y&write.z));
    memory.Store(write.x,value);
}
// layout::pixelWrite for one pixel of a raw segment. The host only sends
// rectangles whose pixels own distinct bits, so threads never write the same
// bits; partial words are merged atomically.
void rawPut(RawSegment s, uint position, uint value) {
    uint x = s.a.z + position % s.b.x, y = s.a.w + position / s.b.x;
    uint base = s.a.x, bw = s.a.y;
    uint pageW = s.d.x & 0xffff, pageH = s.d.x >> 16, colW = s.d.y & 0xffff, colH = s.d.y >> 16;
    uint page = base/32 + (y/pageH)*((bw*64)/pageW) + x/pageW;
    uint blockCols = pageW/colW, blockRows = pageH/colH;
    uint block = layoutTables[(s.d.z & 0xffff) + ((y/colH)%blockRows)*blockCols + (x/colW)%blockCols];
    uint pixel = page*(pageW*pageH) + ((base%32)+block)*(colW*colH) + layoutTables[(s.d.z >> 16) + (y%colH)*colW + x%colW];
    uint bitAddress = pixel*(s.d.w & 0xff) + ((s.d.w >> 8) & 0xff);
    uint address = (bitAddress/8) & (0x400000 - ((s.d.w >> 16) & 0xff));
    uint shift = (address&3)*8 + bitAddress%8;
    uint width = s.d.w >> 24;
    uint mask = (width == 32 ? 0xffffffffu : ((1u << width) - 1u)) << shift;
    uint word = address & ~3u, bits = (value << shift) & mask;
    if (mask == 0xffffffffu) memory.Store(word, bits);
    else { memory.InterlockedAnd(word, ~mask); memory.InterlockedOr(word, bits); }
}
[numthreads(64,1,1)]
void uploadRaw(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID) {
    RawSegment s = rawSegments[rawGroups[group.x]];
    uint unit = (group.x - s.c.w)*64 + local.x;
    if (unit >= s.c.z) return;
    // The unit's bytes, little endian as memcpy reads them.
    uint offset = s.b.w + unit*s.c.x;
    uint lo = rawWords[offset >> 2], hi = rawWords[(offset >> 2) + 1];
    uint shift = (offset & 3)*8;
    uint value = shift ? ((lo >> shift) | (hi << (32 - shift))) : lo;
    if (s.c.x < 4) value &= (1u << (s.c.x*8)) - 1u;
    uint first = s.b.y + unit*s.c.y;
    if (s.c.y == 2) {
        rawPut(s, first, value & 15);
        if (first + 1 < s.b.z) rawPut(s, first + 1, (value >> 4) & 15);
    } else rawPut(s, first, value);
}
uint swizzleCsm1(uint index) { return (index & ~0x18u) | ((index & 0x08u) << 1) | ((index & 0x10u) >> 1); }
// GSCpuBackend::LoadClutUnlocked for one 16-bit CLUT cell.
uint clutCell(uint cell) {
    uint cpsm = clutA.w, csm = clutB.x, csa = clutB.y;
    bool fourBit = clutB.z != 0;
    bool sixteen = cpsm == 2 || cpsm == 10;
    uint count = fourBit ? 16 : 256;
    uint destinationBase = (csa & (sixteen ? 0x1fu : 0x0fu)) << 4;
    bool suffix = csm == 0 && !sixteen && !fourBit;
    uint entry = 0xffffffff;
    bool high = false;
    if (sixteen) {
        uint e = (cell - destinationBase) & 0x1ff;
        if (e < count) entry = e;
    } else {
        uint j = cell & 255;
        high = cell >= 256;
        uint e = suffix ? j : ((j - destinationBase) & 0xff);
        if (e >= (suffix ? destinationBase : 0) && e < count) entry = e;
    }
    if (entry == 0xffffffff) return clutEntry(clutA.x, cell);
    uint sx, sy, width = 1;
    if (csm == 0) { uint si = swizzleCsm1(entry); sx = si & 15; sy = si >> 4; }
    else { width = clutB.w != 0 ? clutB.w : 1; sx = (clutC.x << 4) + entry; sy = clutC.y; }
    uint raw = readTexel(cpsm, clutA.z, width, sx, sy);
    return sixteen ? (raw & 0xffff) : (high ? (raw >> 16) : (raw & 0xffff));
}
[numthreads(64,1,1)]
void loadClut(uint3 thread : SV_DispatchThreadID) {
    if (thread.x >= 256) return;
    uint cell = thread.x * 2;
    cluts.Store(clutA.y * 1024 + thread.x * 4, clutCell(cell) | (clutCell(cell + 1) << 16));
}
)hlsl";
