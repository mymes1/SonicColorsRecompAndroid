#pragma once
#include <cstddef>
#include <cstdint>

// Raw GX register state as written by the command stream.
struct GXState
{
    uint32_t bp[0x100];
    uint32_t cp[0x100];
    uint32_t xfmem[0x1000];     // matrices, lights (0x0000-0x0FFF)
    uint32_t xfregs[0x100];     // 0x1000-0x10FF
    uint32_t bpMaskValue = 0xFFFFFF;   // BP mask (reg 0xFE) for next write
};

extern GXState g_gx;

// Vertex attribute component layout of one vertex as decoded from VCD/VAT.
struct VertexLayout
{
    uint32_t size;
    // per-attribute descriptors
    uint8_t posMtxIdx;      // 0 none, 1 direct
    uint8_t texMtxIdx[8];
    uint8_t pos, nrm, col[2], tex[8];          // 0 none,1 direct,2 idx8,3 idx16
    uint8_t posCnt, posFmt, posShift;
    uint8_t nrmCnt, nrmFmt, nrmIndex3;
    uint8_t colCnt[2], colFmt[2];
    uint8_t texCnt[8], texFmt[8], texShift[8];
    bool byteDequant;
};

void GX_ComputeLayout(int vat, VertexLayout& l);

inline float XFFloat(uint32_t idx)
{
    union { uint32_t u; float f; } c;
    c.u = idx < 0x1000 ? g_gx.xfmem[idx] : g_gx.xfregs[idx - 0x1000];
    return c.f;
}

// CP registers
enum
{
    CP_MATINDEX_A = 0x30,
    CP_MATINDEX_B = 0x40,
    CP_VCD_LO = 0x50,
    CP_VCD_HI = 0x60,
    CP_VAT_A = 0x70,
    CP_VAT_B = 0x80,
    CP_VAT_C = 0x90,
    CP_ARRAY_BASE = 0xA0,
    CP_ARRAY_STRIDE = 0xB0,
};

// XF registers (index into xfregs)
enum
{
    XF_ERROR = 0x00,
    XF_DIAG = 0x01,
    XF_STATE0 = 0x02,
    XF_STATE1 = 0x03,
    XF_CLOCK = 0x04,
    XF_CLIPDISABLE = 0x05,
    XF_SETGPMETRIC = 0x06,
    XF_VTXSPECS = 0x08,
    XF_NUMCOLORS = 0x09,
    XF_AMBIENT0 = 0x0A,
    XF_AMBIENT1 = 0x0B,
    XF_MATERIAL0 = 0x0C,
    XF_MATERIAL1 = 0x0D,
    XF_COLOR0CNTRL = 0x0E,
    XF_COLOR1CNTRL = 0x0F,
    XF_ALPHA0CNTRL = 0x10,
    XF_ALPHA1CNTRL = 0x11,
    XF_DUALTEX = 0x12,
    XF_MATINDEX_A = 0x18,
    XF_MATINDEX_B = 0x19,
    XF_VIEWPORT = 0x1A,     // 6 floats
    XF_PROJECTION = 0x20,   // 6 floats + type
    XF_NUMTEXGENS = 0x3F,
    XF_TEXMTXINFO = 0x40,   // 8
    XF_POSTMTXINFO = 0x50,  // 8
};

// Renderer callbacks (implemented by the D3D backend)
void Render_Draw(int primitive, int vat, uint32_t count, const uint8_t* data, const VertexLayout& layout);
void Render_OnBP(uint32_t reg, uint32_t oldValue, uint32_t newValue);
void Render_OnXF(uint32_t addr, uint32_t count, bool indexed = false);
void Render_OnCP(uint32_t reg);
// True while RunCommands is nested inside CallDisplayList (GPU thread).
bool GX_InDisplayList();
void Render_EFBCopy();
void Render_LoadTLUT();
void Render_PreloadTMEM();
void Render_InvalidateTextureCache();

// A2c/A2d scratch replay isolation (default off / inactive).
void GX_SetPeSuppress(bool on);
bool GX_PeSuppressActive();
void GX_ReplayCommands(const uint8_t* data, size_t n);

void Render_SetReplayIsolation(bool on);
bool Render_ReplayIsolationActive();
void Render_ForceXfDirty();
void Render_ResetHostDrawState();  // flush batches; force XF upload next draw

// Incoming EFB snapshot slots (GPU CopyResource; independent of live EFB).
// slot < 0 means invalid. Capture at pass entry = after post-XFB ClearEFB
// (or immediately after toXFB when the clear bit is off).
int Render_EfbIncomingCapture(uint32_t* outColorHash, uint32_t* outDepthHash,
                              uint32_t* outW, uint32_t* outH);
void Render_EfbIncomingAddRef(int slot);
void Render_EfbIncomingRelease(int slot);
bool Render_EfbIncomingValid(int slot);
uint32_t Render_EfbIncomingColorHash(int slot);
uint32_t Render_EfbIncomingDepthHash(int slot);

// Scratch: seed from incoming (no clear) or clear-to-black (legacy probe).
bool Render_ScratchBeginFromIncoming(int incomingSlot, uint32_t* outW, uint32_t* outH);
// A2g: retainScratchEfbCopies keeps isolation EFB→tex map across sequential frames.
bool Render_ScratchBeginFromIncomingEx(int incomingSlot, bool retainScratchEfbCopies,
                                       uint32_t* outW, uint32_t* outH);
void Render_ScratchDropEfbCopies();
uint32_t Render_ScratchEfbCopyCount();
bool Render_ScratchHasEfbCopy(uint32_t destAddr);
uint64_t Render_ScratchEfbCopyGen(uint32_t destAddr);
uint32_t Render_HashLiveEfbCopy(uint32_t destAddr);
uint32_t Render_HashScratchEfbCopy(uint32_t destAddr);
// A2g selftest: when true under isolation, non-toXFB EFB→tex copies are no-ops
// so retained producer generations are sampled by the consumer frame.
void Render_SetScratchSkipEfbTexCopy(bool skip);
bool Render_ScratchSkipEfbTexCopy();
bool Render_ScratchBeginCleared(uint32_t* outW, uint32_t* outH);
uint32_t Render_ScratchEndHash();          // color fingerprint; ends isolation
uint32_t Render_ScratchEndDepthHash();    // depth fingerprint; call before EndHash
uint32_t Render_HashScratchColor();       // color fingerprint; keeps isolation
uint32_t Render_HashScratchDepth();       // depth fingerprint; keeps isolation
uint32_t Render_HashLiveEfb();
uint32_t Render_HashLiveDepth();
uint32_t Render_HashScratchBlack();

struct RenderEfbCompare
{
    uint32_t width = 0, height = 0;
    uint32_t liveColorHash = 0, scratchColorHash = 0;
    uint32_t liveDepthHash = 0, scratchDepthHash = 0;
    uint32_t colorMismatch = 0;
    uint32_t depthMismatch = 0;   // ~0u => depth compare unavailable
    uint32_t maxChannelDelta = 0;
    uint64_t sumChannelDelta = 0;
    uint32_t comparedPixels = 0;
    bool colorExact = false;
    bool depthExact = false;
    bool depthCompared = false;   // true only if depth pixels were actually compared
};
// Exact CPU compare of live vs scratch color (and optionally depth). Call while both valid,
// before ScratchEndHash tears down isolation (or after with textures still allocated).
// Live sample point: pass-exit EFB (after archived cmds, before post-XFB clear).
bool Render_CompareLiveVsScratch(RenderEfbCompare& out, bool compareDepth);

// Async compare: GPU CopyResource into staging + event query (no Map on submit).
// Ticket 0 = failure. Poll until ready; skipped tickets are never reported as MATCH.
int Render_CompareBeginAsync(bool compareDepth);
bool Render_ComparePoll(int ticket, RenderEfbCompare& out, bool* stillPending);
void Render_CompareCancel(int ticket);
uint32_t Render_CompareQueueDepth();

uint8_t* Render_TmemPtr();
size_t Render_TmemSize();
