#include "font_upscale.h"
#include "config.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace {

bool WriteVtableSlot(void* comObj, size_t slot, void* newFn, void** outOrig) {
    void** vt = *reinterpret_cast<void***>(comObj);
    if (!vt) return false;
    if (vt[slot] == newFn) return true;
    if (outOrig && !*outOrig) *outOrig = vt[slot];
    DWORD oldProt = 0;
    if (!VirtualProtect(&vt[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt)) return false;
    vt[slot] = newFn;
    DWORD dummy = 0;
    VirtualProtect(&vt[slot], sizeof(void*), oldProt, &dummy);
    return true;
}

using FnCreateTexture = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9* self, UINT Width, UINT Height, UINT Levels, DWORD Usage,
    D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle);
using FnSetTexture = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9* self, DWORD Stage, IDirect3DBaseTexture9* pTexture);
using FnSetSamplerState = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9* self, DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value);

using FnTexRelease = ULONG(STDMETHODCALLTYPE*)(IDirect3DTexture9* self);
using FnTexLockRect = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DTexture9* self, UINT Level, D3DLOCKED_RECT* pLockedRect, CONST RECT* pRect, DWORD Flags);
using FnTexUnlockRect = HRESULT(STDMETHODCALLTYPE*)(IDirect3DTexture9* self, UINT Level);

FnCreateTexture   g_origCreateTexture = nullptr;
FnSetTexture      g_origSetTexture = nullptr;
FnSetSamplerState g_origSetSamplerState = nullptr;
FnTexRelease      g_origTexRelease = nullptr;
FnTexLockRect     g_origTexLockRect = nullptr;
FnTexUnlockRect   g_origTexUnlockRect = nullptr;

struct TrackedFontTex {
    IDirect3DDevice9* device = nullptr;
    UINT width = 0;
    UINT height = 0;
    D3DFORMAT format = D3DFMT_UNKNOWN;
    void* lockedBits = nullptr;
    INT lockedPitch = 0;
    bool lockIsWhole = false;
    bool inspected = false;
    IDirect3DTexture9* upscaledTex = nullptr;
};

SRWLOCK g_texLock = SRWLOCK_INIT;
std::unordered_map<IDirect3DTexture9*, TrackedFontTex> g_tracked;
std::unordered_map<IDirect3DBaseTexture9*, IDirect3DTexture9*> g_upscaledLookup;
bool g_enabled = true;
bool g_stage0FontBound = false;
DWORD g_gameMagFilter0 = D3DTEXF_LINEAR;
DWORD g_gameMinFilter0 = D3DTEXF_LINEAR;
int g_logCount = 0;

struct SharedLockGuard {
    SRWLOCK* lk;
    explicit SharedLockGuard(SRWLOCK& l) : lk(&l) { AcquireSRWLockShared(lk); }
    ~SharedLockGuard() { ReleaseSRWLockShared(lk); }
};
struct ExclusiveLockGuard {
    SRWLOCK* lk;
    explicit ExclusiveLockGuard(SRWLOCK& l) : lk(&l) { AcquireSRWLockExclusive(lk); }
    ~ExclusiveLockGuard() { ReleaseSRWLockExclusive(lk); }
};

inline float CatmullRom(float p0, float p1, float p2, float p3, float t) {
    float t2 = t * t;
    float t3 = t2 * t;
    return 0.5f * ((2.0f * p1) +
                   (-p0 + p2) * t +
                   (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                   (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

inline float SampleAlpha(const std::vector<float>& src, int w, int h, int x, int y) {
    x = std::clamp(x, 0, w - 1);
    y = std::clamp(y, 0, h - 1);
    return src[static_cast<size_t>(y) * w + x];
}

// Detects white-RGB glyph atlases (R,G,B near white on non-transparent texels) and extracts normalized alpha.
bool TryExtractFontAlpha(const TrackedFontTex& info, std::vector<float>& outAlpha) {
    const UINT w = info.width;
    const UINT h = info.height;
    const uint8_t* base = static_cast<const uint8_t*>(info.lockedBits);
    if (!base || info.lockedPitch <= 0) return false;

    outAlpha.resize(static_cast<size_t>(w) * h);
    uint32_t nonZero = 0;
    uint32_t whiteMaskCount = 0;
    uint32_t fullyOpaqueCount = 0;

    for (UINT y = 0; y < h; ++y) {
        const uint8_t* row = base + static_cast<size_t>(y) * info.lockedPitch;
        for (UINT x = 0; x < w; ++x) {
            float a = 0.0f;
            bool isWhite = false;
            if (info.format == D3DFMT_A4R4G4B4) {
                uint16_t px = reinterpret_cast<const uint16_t*>(row)[x];
                uint32_t ia = (px >> 12) & 0xF;
                uint32_t ir = (px >> 8) & 0xF;
                uint32_t ig = (px >> 4) & 0xF;
                uint32_t ib = px & 0xF;
                a = static_cast<float>(ia) / 15.0f;
                isWhite = (ir >= 13 && ig >= 13 && ib >= 13);
                if (ia == 15) ++fullyOpaqueCount;
            } else if (info.format == D3DFMT_A8R8G8B8) {
                uint32_t px = reinterpret_cast<const uint32_t*>(row)[x];
                uint32_t ia = (px >> 24) & 0xFF;
                uint32_t ir = (px >> 16) & 0xFF;
                uint32_t ig = (px >> 8) & 0xFF;
                uint32_t ib = px & 0xFF;
                a = static_cast<float>(ia) / 255.0f;
                isWhite = (ir >= 220 && ig >= 220 && ib >= 220);
                if (ia >= 250) ++fullyOpaqueCount;
            } else if (info.format == D3DFMT_A8L8) {
                uint16_t px = reinterpret_cast<const uint16_t*>(row)[x];
                uint32_t ia = (px >> 8) & 0xFF;
                uint32_t il = px & 0xFF;
                a = static_cast<float>(ia) / 255.0f;
                isWhite = (il >= 220);
                if (ia >= 250) ++fullyOpaqueCount;
            } else {
                return false;
            }
            outAlpha[static_cast<size_t>(y) * w + x] = a;
            if (a > 0.01f) {
                ++nonZero;
                if (isWhite) ++whiteMaskCount;
            }
        }
    }

    const uint32_t total = w * h;
    if (nonZero < 24 || fullyOpaqueCount > (total * 90) / 100) {
        return false;
    }
    // Require >= 95% of visible texels to be pure white mask (matches all gamefont_* atlases)
    return (whiteMaskCount * 100 >= nonZero * 95);
}

IDirect3DTexture9* Build4xCrispFontTexture(IDirect3DDevice9* dev, UINT srcW, UINT srcH, const std::vector<float>& srcAlpha) {
    constexpr UINT SCALE = 4;
    const UINT dstW = srcW * SCALE;
    const UINT dstH = srcH * SCALE;

    IDirect3DTexture9* hiTex = nullptr;
    HRESULT hr = g_origCreateTexture(dev, dstW, dstH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &hiTex, nullptr);
    if (FAILED(hr) || !hiTex) {
        return nullptr;
    }

    D3DLOCKED_RECT lr = {};
    hr = g_origTexLockRect ? g_origTexLockRect(hiTex, 0, &lr, nullptr, 0) : hiTex->LockRect(0, &lr, nullptr, 0);
    if (FAILED(hr) || !lr.pBits) {
        if (g_origTexRelease) g_origTexRelease(hiTex);
        else hiTex->Release();
        return nullptr;
    }

    uint8_t* dstBase = static_cast<uint8_t*>(lr.pBits);
    for (UINT dy = 0; dy < dstH; ++dy) {
        float sy = (static_cast<float>(dy) + 0.5f) / static_cast<float>(SCALE) - 0.5f;
        int iy = static_cast<int>(std::floor(sy));
        float ty = sy - static_cast<float>(iy);

        uint32_t* dstRow = reinterpret_cast<uint32_t*>(dstBase + static_cast<size_t>(dy) * lr.Pitch);
        for (UINT dx = 0; dx < dstW; ++dx) {
            float sx = (static_cast<float>(dx) + 0.5f) / static_cast<float>(SCALE) - 0.5f;
            int ix = static_cast<int>(std::floor(sx));
            float tx = sx - static_cast<float>(ix);

            float rowVals[4];
            for (int r = -1; r <= 2; ++r) {
                float p0 = SampleAlpha(srcAlpha, static_cast<int>(srcW), static_cast<int>(srcH), ix - 1, iy + r);
                float p1 = SampleAlpha(srcAlpha, static_cast<int>(srcW), static_cast<int>(srcH), ix,     iy + r);
                float p2 = SampleAlpha(srcAlpha, static_cast<int>(srcW), static_cast<int>(srcH), ix + 1, iy + r);
                float p3 = SampleAlpha(srcAlpha, static_cast<int>(srcW), static_cast<int>(srcH), ix + 2, iy + r);
                rowVals[r + 1] = CatmullRom(p0, p1, p2, p3, tx);
            }
            float val = CatmullRom(rowVals[0], rowVals[1], rowVals[2], rowVals[3], ty);
            val = std::clamp(val, 0.0f, 1.0f);

            // Gentle anti-aliased edge contrast curve: tightens fuzzy edges without eroding thin strokes.
            float edge = std::clamp((val - 0.22f) / (0.76f - 0.22f), 0.0f, 1.0f);
            float sharp = edge * edge * (3.0f - 2.0f * edge);
            float finalA = 0.30f * val + 0.70f * sharp;

            uint32_t ia = static_cast<uint32_t>(std::clamp(finalA * 255.0f + 0.5f, 0.0f, 255.0f));
            dstRow[dx] = (ia << 24) | 0x00FFFFFFu;
        }
    }

    if (g_origTexUnlockRect) g_origTexUnlockRect(hiTex, 0);
    else hiTex->UnlockRect(0);
    return hiTex;
}

HRESULT STDMETHODCALLTYPE Hooked_TexLockRect(
    IDirect3DTexture9* self, UINT Level, D3DLOCKED_RECT* pLockedRect, CONST RECT* pRect, DWORD Flags) {
    HRESULT hr = g_origTexLockRect(self, Level, pLockedRect, pRect, Flags);
    if (SUCCEEDED(hr) && Level == 0 && pLockedRect) {
        ExclusiveLockGuard lk(g_texLock);
        auto it = g_tracked.find(self);
        if (it != g_tracked.end()) {
            it->second.lockedBits = pLockedRect->pBits;
            it->second.lockedPitch = pLockedRect->Pitch;
            it->second.lockIsWhole = (pRect == nullptr);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hooked_TexUnlockRect(IDirect3DTexture9* self, UINT Level) {
    if (g_enabled && Level == 0) {
        TrackedFontTex copy;
        bool shouldInspect = false;
        {
            ExclusiveLockGuard lk(g_texLock);
            auto it = g_tracked.find(self);
            if (it != g_tracked.end() && it->second.lockIsWhole && it->second.lockedBits && !it->second.inspected) {
                it->second.inspected = true;
                copy = it->second;
                shouldInspect = true;
            }
        }

        if (shouldInspect) {
            std::vector<float> alpha;
            if (TryExtractFontAlpha(copy, alpha)) {
                IDirect3DTexture9* hiTex = Build4xCrispFontTexture(copy.device, copy.width, copy.height, alpha);
                if (hiTex) {
                    ExclusiveLockGuard lk(g_texLock);
                    auto it = g_tracked.find(self);
                    if (it != g_tracked.end()) {
                        if (it->second.upscaledTex) {
                            g_upscaledLookup.erase(static_cast<IDirect3DBaseTexture9*>(self));
                            g_origTexRelease(it->second.upscaledTex);
                        }
                        it->second.upscaledTex = hiTex;
                        g_upscaledLookup[static_cast<IDirect3DBaseTexture9*>(self)] = hiTex;
                        LogMsg("[Fonts] Upscaled font atlas %ux%u -> %ux%u (fmt=%u)",
                               copy.width, copy.height, copy.width * 4, copy.height * 4,
                               static_cast<unsigned>(copy.format));
                    } else {
                        g_origTexRelease(hiTex);
                    }
                }
            }
        }
    }

    return g_origTexUnlockRect(self, Level);
}

ULONG STDMETHODCALLTYPE Hooked_TexRelease(IDirect3DTexture9* self) {
    ULONG ref = g_origTexRelease(self);
    if (ref == 0) {
        IDirect3DTexture9* toRelease = nullptr;
        {
            ExclusiveLockGuard lk(g_texLock);
            auto it = g_tracked.find(self);
            if (it != g_tracked.end()) {
                toRelease = it->second.upscaledTex;
                g_upscaledLookup.erase(static_cast<IDirect3DBaseTexture9*>(self));
                g_tracked.erase(it);
            }
        }
        if (toRelease) {
            g_origTexRelease(toRelease);
        }
    }
    return ref;
}

HRESULT STDMETHODCALLTYPE Hooked_CreateTexture(
    IDirect3DDevice9* self, UINT Width, UINT Height, UINT Levels, DWORD Usage,
    D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle) {
    HRESULT hr = g_origCreateTexture(self, Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
    if (SUCCEEDED(hr) && ppTexture && *ppTexture && g_enabled) {
        bool isCandidateFormat = (Format == D3DFMT_A4R4G4B4 || Format == D3DFMT_A8R8G8B8 || Format == D3DFMT_A8L8);
        bool isRenderTarget = (Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) != 0;
        if (!isRenderTarget && isCandidateFormat && Width >= 64 && Width <= 256 && Height >= 64 && Height <= 256) {
            IDirect3DTexture9* tex = *ppTexture;
            WriteVtableSlot(tex, 2,  reinterpret_cast<void*>(Hooked_TexRelease),    reinterpret_cast<void**>(&g_origTexRelease));
            WriteVtableSlot(tex, 19, reinterpret_cast<void*>(Hooked_TexLockRect),   reinterpret_cast<void**>(&g_origTexLockRect));
            WriteVtableSlot(tex, 20, reinterpret_cast<void*>(Hooked_TexUnlockRect), reinterpret_cast<void**>(&g_origTexUnlockRect));

            ExclusiveLockGuard lk(g_texLock);
            TrackedFontTex info;
            info.device = self;
            info.width = Width;
            info.height = Height;
            info.format = Format;
            g_tracked[tex] = info;
            if (g_logCount < 16) {
                ++g_logCount;
                LogMsg("[Fonts] Candidate texture %ux%u fmt=%u pool=%u levels=%u",
                       Width, Height, static_cast<unsigned>(Format), static_cast<unsigned>(Pool), Levels);
            }
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hooked_SetSamplerState(
    IDirect3DDevice9* self, DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) {
    if (Sampler == 0) {
        if (Type == D3DSAMP_MAGFILTER) {
            g_gameMagFilter0 = Value;
            if (g_stage0FontBound) Value = D3DTEXF_LINEAR;
        } else if (Type == D3DSAMP_MINFILTER) {
            g_gameMinFilter0 = Value;
            if (g_stage0FontBound) Value = D3DTEXF_LINEAR;
        }
    }
    return g_origSetSamplerState(self, Sampler, Type, Value);
}

HRESULT STDMETHODCALLTYPE Hooked_SetTexture(
    IDirect3DDevice9* self, DWORD Stage, IDirect3DBaseTexture9* pTexture) {
    if (g_enabled && Stage == 0) {
        IDirect3DTexture9* replacement = nullptr;
        if (pTexture) {
            SharedLockGuard lk(g_texLock);
            auto it = g_upscaledLookup.find(pTexture);
            if (it != g_upscaledLookup.end()) {
                replacement = it->second;
            }
        }

        if (replacement) {
            g_stage0FontBound = true;
            HRESULT hr = g_origSetTexture(self, Stage, replacement);
            if (g_origSetSamplerState) {
                g_origSetSamplerState(self, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                g_origSetSamplerState(self, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            }
            return hr;
        } else if (g_stage0FontBound) {
            g_stage0FontBound = false;
            if (g_origSetSamplerState) {
                g_origSetSamplerState(self, 0, D3DSAMP_MAGFILTER, g_gameMagFilter0);
                g_origSetSamplerState(self, 0, D3DSAMP_MINFILTER, g_gameMinFilter0);
            }
        }
    }
    return g_origSetTexture(self, Stage, pTexture);
}

} // namespace

void InstallFontUpscalerHooks(IDirect3DDevice9* device, bool enabled) {
    g_enabled = enabled;
    if (!device || !enabled) return;

    WriteVtableSlot(device, 23, reinterpret_cast<void*>(Hooked_CreateTexture),   reinterpret_cast<void**>(&g_origCreateTexture));
    WriteVtableSlot(device, 65, reinterpret_cast<void*>(Hooked_SetTexture),      reinterpret_cast<void**>(&g_origSetTexture));
    WriteVtableSlot(device, 69, reinterpret_cast<void*>(Hooked_SetSamplerState), reinterpret_cast<void**>(&g_origSetSamplerState));
    LogMsg("[Fonts] Installed 4x crisp font upscaler hooks");
}

void OnDeviceResetForFontUpscaler() {
    g_stage0FontBound = false;
    g_gameMagFilter0 = D3DTEXF_LINEAR;
    g_gameMinFilter0 = D3DTEXF_LINEAR;
}
