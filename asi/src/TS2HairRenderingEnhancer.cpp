// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 evannamari
// Developed with the assistance of Claude (Anthropic).
//
// This program is free software: you can redistribute it and/or modify it under the terms of the
// GNU General Public License as published by the Free Software Foundation, either version 3 of
// the License, or (at your option) any later version. It is distributed in the hope that it will
// be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
// You should have received a copy of the GNU General Public License along with this program.
// If not, see <https://www.gnu.org/licenses/>.

// TS2HairRenderingEnhancer.cpp -- The Sims 2: soft, detailed alpha edges for hair (optional triangle depth sorting).  Build as x86 DLL -> rename to .asi
//
// For every draw call that matches the vanilla hair signature (alpha blend src/invSrc, ztest+zwrite, alphatest GREATER ref=60)
// the group's triangles are optionally sorted far -> near by camera depth (CPU replica of the VS skinning, TS2HairRenderingEnhancer.h;
// the author's GROUP order is untouched) and the group is drawn in two passes:
//
//   Mode 1 "split" (default)
//     S  solid  : alpha >  SolidAlphaRef, zwrite ON (as in vanilla)  -> same depth footprint as vanilla, the z-buffer keeps
//                 solid parts in the right order even where the triangle sort is imperfect
//     F  fringe : alpha <= SolidAlphaRef, zwrite OFF, ztest ON       -> the faint texture detail ("alphaTest 0"), hidden
//                 behind solid hair, never writes depth, so it cannot cut holes into other transparent objects
//   Mode 0 "legacy"
//     colour pass (zwrite OFF, alphaRef = SoftAlphaRef) followed by a depth-only pass (colour writes OFF, zwrite ON)
//
// Sorting is optional (default off). If it is off or impossible for a draw, the group is still drawn with both passes
// in the original index order. Only draws that do not look like hair are passed through untouched.
//
// Hotkeys (Ctrl+Shift+): F8 = whole mod on/off (A/B against vanilla)   F7 = triangle sorting on/off   F6 = mode 0/1
// Settings: TS2HairRenderingEnhancer.ini next to the .asi.   Log: TS2HairRenderingEnhancer.log

#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "TS2HairRenderingEnhancer.h"

#define HS_VERSION "0.5.0"

typedef HRESULT (__stdcall *CreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT (__stdcall *Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT (__stdcall *DIP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);

static CreateDevice_t o_CreateDevice;
static Present_t      o_Present;
static DIP_t          o_DIP;

// ---------------------------------------------------------------- config / log
struct Config {
    DWORD sigRef    = 60;     // alpha ref that identifies vanilla hair draws
    int   mode      = 1;      // 1 = split solid/fringe, 0 = legacy colour pass + depth fill
    DWORD solidRef  = 60;     // mode 1: alpha > solidRef is "solid" (depth written), alpha <= solidRef is "fringe"
    DWORD softRef   = 0;      // mode 0: alpha ref of the colour pass (0 = alphaTest 0)
    DWORD depthRef  = 60;     // mode 0: alpha ref of the depth-only pass
    bool  depthFill = true;   // mode 0
    bool  sortTris  = false;   // off by default: see TS2HairRenderingEnhancer.ini
    bool  enabled   = true;
    bool  writeLog  = true;   // WriteLog=0: no log file is created
} g_cfg;

static HMODULE g_self;
static FILE*   g_log;
static bool    g_devicePatched;

static void L(const char* f, ...)
{
    if (!g_log) return;
    va_list a; va_start(a, f); vfprintf(g_log, f, a); va_end(a);
    fflush(g_log);
}

static void LoadConfig()
{
    char path[MAX_PATH];
    GetModuleFileNameA(g_self, path, MAX_PATH);
    char* dot = strrchr(path, '.');
    if (dot) strcpy(dot, ".ini");
    g_cfg.sigRef    = GetPrivateProfileIntA("Hair", "SignatureAlphaRef", 60, path);
    g_cfg.mode      = GetPrivateProfileIntA("Hair", "Mode",              1,  path) ? 1 : 0;
    g_cfg.solidRef  = GetPrivateProfileIntA("Hair", "SolidAlphaRef",     60, path);
    g_cfg.softRef   = GetPrivateProfileIntA("Hair", "SoftAlphaRef",      0,  path);
    g_cfg.depthRef  = GetPrivateProfileIntA("Hair", "DepthAlphaRef",     60, path);
    g_cfg.depthFill = GetPrivateProfileIntA("Hair", "DepthFill",         1,  path) != 0;
    g_cfg.sortTris  = GetPrivateProfileIntA("Hair", "SortTriangles",     0,  path) != 0;
    g_cfg.enabled   = GetPrivateProfileIntA("Hair", "Enabled",           1,  path) != 0;
    g_cfg.writeLog  = GetPrivateProfileIntA("Hair", "WriteLog",          1,  path) != 0;
}

// ---------------------------------------------------------------- stats
static LARGE_INTEGER g_qpf;
static unsigned g_frame, g_processed, g_sortedDraws, g_fallback, g_passthrough;
static double   g_cpuMs, g_maxMs;
static unsigned g_reasonCount[16];
static const char* g_reasonName[16];

static void CountReason(const char* why)
{
    for (int i = 0; i < 16; ++i) {
        if (g_reasonName[i] == why) { ++g_reasonCount[i]; return; }
        if (!g_reasonName[i]) { g_reasonName[i] = why; g_reasonCount[i] = 1; L("fallback reason (first time): %s\n", why); return; }
    }
}

// ---------------------------------------------------------------- D3D helpers
static DWORD RS(IDirect3DDevice9* d, D3DRENDERSTATETYPE s) { DWORD v = 0xFFFFFFFF; d->GetRenderState(s, &v); return v; }

// vanilla hair signature (SimHairAlphaState, blend branch)
static bool IsHair(IDirect3DDevice9* d)
{
    return RS(d, D3DRS_ALPHATESTENABLE) == TRUE
        && RS(d, D3DRS_ALPHAREF)        == g_cfg.sigRef
        && RS(d, D3DRS_ALPHAFUNC)       == D3DCMP_GREATER
        && RS(d, D3DRS_ALPHABLENDENABLE)== TRUE
        && RS(d, D3DRS_SRCBLEND)        == D3DBLEND_SRCALPHA
        && RS(d, D3DRS_DESTBLEND)       == D3DBLEND_INVSRCALPHA
        && RS(d, D3DRS_ZENABLE)         != D3DZB_FALSE
        && RS(d, D3DRS_ZWRITEENABLE)    == TRUE;
}

// read-only lock of a vertex stream, released automatically
struct StreamLock {
    IDirect3DVertexBuffer9* vb = 0; UINT off = 0, stride = 0; BYTE* data = 0; bool locked = false;
    bool Open(IDirect3DDevice9* d, UINT stream, INT base, UINT nVerts) {
        if (FAILED(d->GetStreamSource(stream, &vb, &off, &stride)) || !vb || stride == 0) return false;
        D3DVERTEXBUFFER_DESC desc;
        if (FAILED(vb->GetDesc(&desc))) return false;
        if ((unsigned long long)off + (unsigned long long)(base + nVerts) * stride > desc.Size) return false;
        void* p = 0;
        if (FAILED(vb->Lock(0, 0, &p, D3DLOCK_READONLY)) || !p) return false;
        data = (BYTE*)p; locked = true;
        return true;
    }
    ~StreamLock() { if (locked) vb->Unlock(); if (vb) vb->Release(); }
};

// our own index buffer for sorted results: MANAGED (survives device Reset), same usage flags the engine uses for its own IBs
static IDirect3DIndexBuffer9* g_ib;
static IDirect3DDevice9*      g_ibDevice;
static const UINT kIbCapacity = 2000000;     // indices (16-bit) -> 4 MB managed, ~39 heavy (17k tri) hairs per frame
static UINT g_ibCursor;                      // reset every frame

static bool EnsureIB(IDirect3DDevice9* d)
{
    if (g_ib && g_ibDevice == d) return true;
    if (g_ib) { g_ib->Release(); g_ib = 0; }
    if (FAILED(d->CreateIndexBuffer(kIbCapacity * 2, D3DUSAGE_SOFTWAREPROCESSING, D3DFMT_INDEX16, D3DPOOL_MANAGED, &g_ib, 0))) { g_ib = 0; return false; }
    g_ibDevice = d;
    return true;
}

static hs::Scratch g_scratch;
static std::vector<float> g_keys;

// Builds the far->near sorted copy of this group's indices in our own IB.  Never issues a draw call.
// false = sorting not possible for this draw (reason in *why) -> the caller still draws the group (unsorted) with both passes.
static bool BuildSorted(IDirect3DDevice9* d, IDirect3DIndexBuffer9* origIB, INT base, UINT minIdx, UINT nVerts, UINT start, UINT prims,
                        const char** why, UINT* sortedStart)
{
    if (minIdx != 0 || nVerts == 0 || base < 0 || nVerts > 65535) { *why = "unexpected draw args"; return false; }
    if (g_ibCursor + prims * 3 > kIbCapacity) { *why = "sorted index buffer full (drawn unsorted)"; return false; }

    // ---- vertex declaration: need float3 POSITION, float3 BLENDWEIGHT, ubyte4 BLENDINDICES
    struct { int stream, off; } pos = {-1,0}, wgt = {-1,0}, idx = {-1,0};
    {
        IDirect3DVertexDeclaration9* decl = 0;
        if (FAILED(d->GetVertexDeclaration(&decl)) || !decl) { *why = "no vertex declaration"; return false; }
        D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1]; UINT n = 0;
        bool ok = SUCCEEDED(decl->GetDeclaration(el, &n));
        decl->Release();
        if (!ok) { *why = "GetDeclaration failed"; return false; }
        for (UINT i = 0; i < n && el[i].Stream != 0xFF; ++i) {
            if (el[i].UsageIndex != 0) continue;
            if (el[i].Usage == D3DDECLUSAGE_POSITION     && el[i].Type == D3DDECLTYPE_FLOAT3) { pos.stream = el[i].Stream; pos.off = el[i].Offset; }
            if (el[i].Usage == D3DDECLUSAGE_BLENDWEIGHT  && el[i].Type == D3DDECLTYPE_FLOAT3) { wgt.stream = el[i].Stream; wgt.off = el[i].Offset; }
            if (el[i].Usage == D3DDECLUSAGE_BLENDINDICES && el[i].Type == D3DDECLTYPE_UBYTE4) { idx.stream = el[i].Stream; idx.off = el[i].Offset; }
        }
        if (pos.stream < 0 || wgt.stream < 0 || idx.stream < 0 || wgt.stream != idx.stream) { *why = "vertex format is not pos+float3 weights+ubyte4 indices"; return false; }
        if (pos.stream > 3 || wgt.stream > 3) { *why = "stream index"; return false; }
    }

    D3DINDEXBUFFER_DESC idesc;
    if (FAILED(origIB->GetDesc(&idesc)) || idesc.Format != D3DFMT_INDEX16) { *why = "index format is not 16 bit"; return false; }
    if ((unsigned long long)(start + prims * 3) * 2 > idesc.Size) { *why = "index range outside buffer"; return false; }
    if (!EnsureIB(d)) { *why = "cannot create index buffer"; return false; }

    // constants: cameraToClip c0..c3 (we need row c3) and the 180-register bone palette at c56
    float proj[16], palette[180 * 4];
    if (FAILED(d->GetVertexShaderConstantF(0, proj, 4)) || FAILED(d->GetVertexShaderConstantF(56, palette, 180))) { *why = "GetVertexShaderConstantF failed"; return false; }
    const float* c3 = proj + 12;
    if (c3[0] == 0 && c3[1] == 0 && c3[2] == 0 && c3[3] == 0) { *why = "cameraToClip row 3 is zero"; return false; }

    StreamLock sp, sw;
    if (!sp.Open(d, pos.stream, base, nVerts)) { *why = "cannot lock position stream"; return false; }
    StreamLock* sk = &sw;
    if (wgt.stream == pos.stream) sk = &sp;
    else if (!sw.Open(d, wgt.stream, base, nVerts)) { *why = "cannot lock skin stream"; return false; }

    const BYTE* posPtr = sp.data + sp.off + (size_t)base * sp.stride + pos.off;
    const BYTE* wPtr   = sk->data + sk->off + (size_t)base * sk->stride + wgt.off;
    const BYTE* iPtr   = sk->data + sk->off + (size_t)base * sk->stride + idx.off;

    g_keys.resize(nVerts);
    if (!hs::ComputeDepthKeys(posPtr, sp.stride, wPtr, iPtr, sk->stride, nVerts, palette, c3, g_keys.data())) { *why = "skinning data rejected (bone index >= 60 or NaN)"; return false; }

    void* ip = 0;
    if (FAILED(origIB->Lock(0, 0, &ip, D3DLOCK_READONLY)) || !ip) { *why = "cannot lock source index buffer"; return false; }
    void* op = 0;
    bool ok = false;
    if (SUCCEEDED(g_ib->Lock(g_ibCursor * 2, prims * 3 * 2, &op, 0)) && op) {
        ok = hs::SortTriangles((const uint16_t*)ip + start, prims, g_keys.data(), nVerts, (uint16_t*)op, g_scratch);
        g_ib->Unlock();
    }
    origIB->Unlock();
    if (!ok) { *why = "sort rejected the triangles (index >= NumVertices?)"; return false; }
    *sortedStart = g_ibCursor;
    g_ibCursor += prims * 3;
    return true;
}

// returns false -> caller issues the ORIGINAL draw call.  On true the draw has been fully issued.
// Sorting is optional: if it is off or impossible, the group is STILL drawn with both passes (original index order),
// so every hair draw gets the same soft edge instead of some groups silently falling back to the vanilla hard cut.
static bool ProcessHair(IDirect3DDevice9* d, INT base, UINT minIdx, UINT nVerts, UINT start, UINT prims, const char** why, bool* usedSort)
{
    *usedSort = false;
    if (prims == 0 || nVerts == 0 || base < 0) { *why = "unexpected draw args"; return false; }

    IDirect3DIndexBuffer9* origIB = 0;     // only fetched when sorting is requested; restored after our draws
    UINT sortedStart = 0;
    bool haveSorted = false;

    if (g_cfg.sortTris) {
        const char* sortWhy = 0;
        if (SUCCEEDED(d->GetIndices(&origIB)) && origIB && BuildSorted(d, origIB, base, minIdx, nVerts, start, prims, &sortWhy, &sortedStart)) {
            haveSorted = true; *usedSort = true;
        } else CountReason(sortWhy ? sortWhy : "no index buffer");
    }

    // ---- issue the passes
    const DWORD zw = RS(d, D3DRS_ZWRITEENABLE), aref = RS(d, D3DRS_ALPHAREF), afunc = RS(d, D3DRS_ALPHAFUNC), cw = RS(d, D3DRS_COLORWRITEENABLE);

    auto drawGroup = [&](bool sorted) {
        if (sorted) { d->SetIndices(g_ib); o_DIP(d, D3DPT_TRIANGLELIST, base, minIdx, nVerts, sortedStart, prims); d->SetIndices(origIB); }
        else        { o_DIP(d, D3DPT_TRIANGLELIST, base, minIdx, nVerts, start, prims); }
    };

    if (g_cfg.mode == 1) {
        d->SetRenderState(D3DRS_ALPHAREF, g_cfg.solidRef);                 // S: solid, identical to vanilla (zwrite stays ON)
        drawGroup(haveSorted);
        d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);                      // F: faint fringe, no depth write
        d->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_LESSEQUAL);
        d->SetRenderState(D3DRS_ALPHAREF, g_cfg.solidRef);
        drawGroup(haveSorted);
    } else {
        d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        d->SetRenderState(D3DRS_ALPHAREF, g_cfg.softRef);
        drawGroup(haveSorted);
        if (g_cfg.depthFill) {
            d->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
            d->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
            d->SetRenderState(D3DRS_ALPHAREF, g_cfg.depthRef);
            drawGroup(false);
        }
    }

    d->SetRenderState(D3DRS_COLORWRITEENABLE, cw);
    d->SetRenderState(D3DRS_ALPHAFUNC, afunc);
    d->SetRenderState(D3DRS_ALPHAREF, aref);
    d->SetRenderState(D3DRS_ZWRITEENABLE, zw);
    if (origIB) origIB->Release();
    return true;
}

// ---------------------------------------------------------------- hooks
static HRESULT __stdcall H_DIP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT base, UINT minIdx, UINT nVerts, UINT start, UINT prims)
{
    if (g_cfg.enabled && t == D3DPT_TRIANGLELIST && IsHair(d)) {
        LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
        const char* why = 0; bool usedSort = false;
        const bool done = ProcessHair(d, base, minIdx, nVerts, start, prims, &why, &usedSort);
        QueryPerformanceCounter(&b);
        const double ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
        g_cpuMs += ms; if (ms > g_maxMs) g_maxMs = ms;
        if (done) { ++g_processed; if (usedSort) ++g_sortedDraws; return D3D_OK; }
        ++g_fallback; CountReason(why ? why : "unknown");
    } else if (g_cfg.enabled) ++g_passthrough;
    return o_DIP(d, t, base, minIdx, nVerts, start, prims);
}

static void Beep2(bool on) { MessageBeep(on ? MB_ICONASTERISK : MB_ICONEXCLAMATION); }

static HRESULT __stdcall H_Present(IDirect3DDevice9* d, const RECT* a, const RECT* b, HWND w, const RGNDATA* r)
{
    g_ibCursor = 0;
    ++g_frame;

    static bool p6 = false, p7 = false, p8 = false;
    const bool mods = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000);
    const bool k6 = mods && (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    const bool k7 = mods && (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    const bool k8 = mods && (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (k8 && !p8) { g_cfg.enabled  = !g_cfg.enabled;  L("frame %u: mod %s\n", g_frame, g_cfg.enabled ? "ON" : "OFF (vanilla)"); Beep2(g_cfg.enabled); }
    if (k7 && !p7) { g_cfg.sortTris = !g_cfg.sortTris; L("frame %u: triangle sorting %s\n", g_frame, g_cfg.sortTris ? "ON" : "OFF"); Beep2(g_cfg.sortTris); }
    if (k6 && !p6) { g_cfg.mode = 1 - g_cfg.mode; L("frame %u: mode %d (%s)\n", g_frame, g_cfg.mode, g_cfg.mode ? "split solid/fringe" : "legacy colour+depth"); Beep2(g_cfg.mode == 1); }
    p6 = k6; p7 = k7; p8 = k8;

    if (g_frame % 600 == 0 && (g_processed || g_fallback)) {
        L("frame %u: hair draws processed=%u (with sorting=%u) fallback=%u  CPU per hair draw: avg=%.3f ms max=%.3f ms\n", g_frame, g_processed, g_sortedDraws, g_fallback,
          g_processed + g_fallback ? g_cpuMs / (g_processed + g_fallback) : 0.0, g_maxMs);
        for (int i = 0; i < 16 && g_reasonName[i]; ++i) L("   fallback: %u x %s\n", g_reasonCount[i], g_reasonName[i]);
    }
    return o_Present(d, a, b, w, r);
}

static void PatchVtbl(void** vt, int idx, void* hook, void** orig)
{
    DWORD old;
    VirtualProtect(&vt[idx], sizeof(void*), PAGE_READWRITE, &old);
    *orig = vt[idx];
    vt[idx] = hook;
    VirtualProtect(&vt[idx], sizeof(void*), old, &old);
}

static HRESULT __stdcall H_CreateDevice(IDirect3D9* d3d, UINT ad, D3DDEVTYPE ty, HWND w, DWORD fl, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out)
{
    HRESULT hr = o_CreateDevice(d3d, ad, ty, w, fl, pp, out);
    if (SUCCEEDED(hr) && out && *out && !g_devicePatched) {
        void** vt = *(void***)(*out);
        PatchVtbl(vt, 17, (void*)H_Present, (void**)&o_Present);       // IDirect3DDevice9::Present
        PatchVtbl(vt, 82, (void*)H_DIP,     (void**)&o_DIP);           // IDirect3DDevice9::DrawIndexedPrimitive
        g_devicePatched = true;
        L("device created (flags=0x%08X) -> Present + DrawIndexedPrimitive hooked\n", fl);
    }
    return hr;
}

static DWORD WINAPI InitThread(LPVOID)
{
    char path[MAX_PATH];
    GetModuleFileNameA(g_self, path, MAX_PATH);
    char* dot = strrchr(path, '.');
    if (dot) strcpy(dot, ".log");
    QueryPerformanceFrequency(&g_qpf);
    LoadConfig();
    if (g_cfg.writeLog) g_log = fopen(path, "w");          // WriteLog=0 -> g_log stays null and L() does nothing
    L("TS2 Hair Rendering Enhancer v" HS_VERSION " started: enabled=%d mode=%d sort=%d signatureRef=%u solidRef=%u (legacy: softRef=%u depthFill=%d depthRef=%u)\n",
      g_cfg.enabled, g_cfg.mode, g_cfg.sortTris, g_cfg.sigRef, g_cfg.solidRef, g_cfg.softRef, g_cfg.depthFill, g_cfg.depthRef);

    HMODULE h = LoadLibraryA("d3d9.dll");
    typedef IDirect3D9* (WINAPI *Create9_t)(UINT);
    Create9_t create9 = h ? (Create9_t)GetProcAddress(h, "Direct3DCreate9") : 0;
    if (!create9) { L("Direct3DCreate9 not found\n"); return 0; }
    IDirect3D9* d3d = create9(D3D_SDK_VERSION);                         // temporary object, only to reach the IDirect3D9 vtable
    if (!d3d) { L("Direct3DCreate9 failed\n"); return 0; }
    PatchVtbl(*(void***)d3d, 16, (void*)H_CreateDevice, (void**)&o_CreateDevice);
    d3d->Release();
    L("IDirect3D9::CreateDevice hooked\n");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) { g_self = inst; DisableThreadLibraryCalls(inst); CreateThread(0, 0, InitThread, 0, 0, 0); }
    return TRUE;
}
