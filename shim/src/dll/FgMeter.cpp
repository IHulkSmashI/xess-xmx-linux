// FgMeter.cpp - even flips for XeSS frame generation under Proton.
//
// On Windows XeSS-FG hands its presents to the Intel driver back to back, and the driver spaces them: XeSS-FG tells it
// how many generated frames come with the next real one (INTC_D3D12_SetNumGeneratedFrames) and which present is which
// (SetPresentSequenceNumber), and reads back when they were shown (GetDisplayTelemetry). Under Proton there is no such
// driver: vkd3d-proton / DXVK present each frame the moment XeSS-FG calls Present, so with VRR (no V-Sync to spread
// them) the display gets each rendered frame's generated frames and the real one within a fraction of a millisecond,
// then nothing until the next rendered frame - a burst per frame, seen as stutter. A frame cap only hides it, V-Sync
// costs latency, and vkd3d-proton's own present timing (VK_EXT_present_timing) needs present wait 2, which the package
// disables, and is only used for V-Sync'ed presents.
//
// So the shim spaces them itself, on the swap chain XeSS-FG presents to (the one it creates in
// xefgSwapChainD3D12InitFromSwapChain(Desc), found by hooking the DXGI factory for the length of that call): each
// Present is held until the previous one + real-frame interval / presents per real frame, never longer than one
// interval after it arrived (FgPacer.h has the schedule and where it comes from). The wait is a short sleep
// (NtDelayExecution, stopping short by the late wake-ups seen recently) and a spin to the target on QPC.
//
// IGDEXT_FG_METER=1 turns it on; 0 or unset leaves it off. Off by default: it changes when every frame of every game
// with XeSS frame generation reaches the screen, and it is new; it becomes the default once it has been tested on
// hardware. With it off and IGDEXT_XELL_LOG unset nothing here is installed. With IGDEXT_XELL_LOG=1 the hooks go in
// either way and C:\igdext_xell.log gets a pacing summary every 2 s ("fgmeter: ..."), with the meter on or off, so that
// both can be compared: "short" counts the intervals under half the even spacing (the bursts).
// Only while frame generation is on (xefgSwapChainSetEnabled / xellSetFgEnabled): off, every present goes straight
// through, and the schedule starts over when it comes back.
#include "Stdafx.h"
#include "FgMeter.h"
#include "FgPacer.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <intrin.h>

namespace
{
int g_mode = -1;   // -1 not read, 0 off, 1 on

typedef HRESULT (STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* PFN_Present1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateSwapChain)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateForHwnd)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                       const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateForComposition)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*,
                                                              IDXGIOutput*, IDXGISwapChain1**);
typedef LONG (NTAPI* PFN_NtDelayExecution)(BOOLEAN alertable, PLARGE_INTEGER interval);

// vtable slots (the COM interface order of dxgi.h / dxgi1_2.h): IUnknown 0-2, IDXGIObject 3-6
enum
{
    kSlotPresent = 8,                  // IDXGISwapChain: IDXGIDeviceSubObject::GetDevice 7, Present 8
    kSlotPresent1 = 22,                // IDXGISwapChain1: GetDesc1 18 ... GetCoreWindow 21, Present1 22
    kSlotCreateSwapChain = 10,         // IDXGIFactory: EnumAdapters 7 ... CreateSwapChain 10
    kSlotCreateForHwnd = 15,           // IDXGIFactory2: IsWindowedStereoEnabled 14, CreateSwapChainForHwnd 15
    kSlotCreateForComposition = 24,    // ... UnregisterOcclusionStatus 23, CreateSwapChainForComposition 24
};

double QpcMs()
{
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double) (t.QuadPart / f.QuadPart) * 1000.0 + (double) (t.QuadPart % f.QuadPart) * 1000.0 / (double) f.QuadPart;
}

// until QPC reaches the time (ms of QpcMs): NtDelayExecution (100 ns units) for all but the late wake-up seen recently,
// a maximum that decays, then a spin
void WaitUntilMs(double targetMs)
{
    static PFN_NtDelayExecution delay = (PFN_NtDelayExecution) GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtDelayExecution");
    static thread_local double lateMs = 1.0;
    const double now = QpcMs(), left = targetMs - now;
    if (left <= 0) return;
    const double slack = std::min(2.0, std::max(0.3, lateMs + 0.2));
    if (left > slack)
    {
        const double ask = left - slack;
        if (delay) { LARGE_INTEGER rel; rel.QuadPart = -(LONGLONG) (ask * 10000.0); delay(FALSE, &rel); }
        else Sleep((DWORD) ask);
        lateMs = std::max(QpcMs() - now - ask, lateMs * 0.98);
    }
    while (QpcMs() < targetMs) YieldProcessor();
}

// a vtable slot, written in place (vtables are read-only data); returns what was there
void* PatchSlot(void** vtbl, int slot, void* hook)
{
    void* old = vtbl[slot];
    if (old == hook) return old;
    DWORD prot;
    if (!VirtualProtect(&vtbl[slot], sizeof(void*), PAGE_READWRITE, &prot)) return nullptr;
    InterlockedExchangePointer(&vtbl[slot], hook);
    VirtualProtect(&vtbl[slot], sizeof(void*), prot, &prot);
    return old;
}

SRWLOCK g_patchLock = SRWLOCK_INIT;

// a vtable slot that points into this module keeps it mapped until the process exits (games free and load the shim
// again at startup); false: not pinned, nothing is patched
bool PinSelf()
{
    static bool pinned = false;
    HMODULE self = nullptr;
    if (!pinned && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR) &PinSelf, &self))
        pinned = true;
    return pinned;
}

// ---- the swap chain classes: the originals of each patched vtable (one class in practice: DXVK's DXGI swap chain) --
struct ScVtbl { void** vtbl; PFN_Present present; PFN_Present1 present1; };
ScVtbl g_sc[8];
volatile LONG g_nSc = 0;

const ScVtbl* FindSc(void* obj)
{
    void** vtbl = *(void***) obj;
    const LONG n = g_nSc;
    for (LONG i = 0; i < n; ++i) if (g_sc[i].vtbl == vtbl) return &g_sc[i];
    // a vtable copied by another hook, pointing at ours: the first class seen is the one it copied
    static volatile LONG said = 0;
    if (n && InterlockedExchange(&said, 1) == 0) XellLogF("fgmeter: present on an unknown vtable %p, using the originals of %p", vtbl, g_sc[0].vtbl);
    return n ? &g_sc[0] : nullptr;
}

// ---- what is paced -----------------------------------------------------------------------------------------------
struct Stats
{
    double t0 = 0, lastIn = 0, lastOut = 0;
    int n = 0, nIv = 0, shortIn = 0, shortOut = 0, waited = 0, capped = 0, late = 0;
    double sumIn = 0, sumIn2 = 0, maxIn = 0, sumOut = 0, sumOut2 = 0, maxOut = 0, sumWait = 0, maxWait = 0;
    double sumSpacing = 0, sumInterval = 0, sumPerReal = 0; int nPlan = 0;
};

struct Target
{
    void* ctx = nullptr;
    void* sc = nullptr;     // the swap chain as created (IDXGISwapChain or IDXGISwapChain1)
    void* sc1 = nullptr;    // its IDXGISwapChain1 pointer, if another one
    SRWLOCK lock = SRWLOCK_INIT;
    FgPacer pacer;
    int fgOn = -1;          // the state the schedule was made for
    Stats st;
};
Target g_targets[4];
LONG g_nextTarget = 0;
volatile LONG g_noted = -1;

Target* FindTarget(void* self)
{
    for (Target& t : g_targets) if (self && (t.sc == self || t.sc1 == self)) return &t;
    return nullptr;
}

void Summary(Target& t, double now, bool metering)
{
    Stats& s = t.st;
    const double secs = (now - s.t0) / 1000.0;
    const int iv = s.nIv ? s.nIv : 1;
    const double inAvg = s.sumIn / iv, outAvg = s.sumOut / iv;
    const double inSd = std::sqrt(std::max(0.0, s.sumIn2 / iv - inAvg * inAvg));
    const double outSd = std::sqrt(std::max(0.0, s.sumOut2 / iv - outAvg * outAvg));
    const int np = s.nPlan ? s.nPlan : 1;
    XellLogF("fgmeter %p: %s, %.1f s, %d presents (%.1f/s), real frame %.2f ms, %.2f presents per real frame, spacing %.2f ms"
             " | in: interval avg %.2f sd %.2f max %.2f ms, short %d%% | out: avg %.2f sd %.2f max %.2f ms, short %d%%"
             " | waited %d x avg %.2f max %.2f ms, capped %d, late %d",
             t.sc, metering ? "meter on" : "meter off (measuring only)", secs, s.n, s.n / (secs > 0 ? secs : 1),
             s.sumInterval / np, s.sumPerReal / np, s.sumSpacing / np,
             inAvg, inSd, s.maxIn, 100 * s.shortIn / iv, outAvg, outSd, s.maxOut, 100 * s.shortOut / iv,
             s.waited, s.waited ? s.sumWait / s.waited : 0.0, s.maxWait, s.capped, s.late);
    s = Stats();
    s.t0 = now;
}

// before the real Present of a target: wait for its turn (pacing on), and count
void Meter(Target& t)
{
    const bool metering = FgMeterOn();
    const int fg = XefgFrameGenerationState() == 1;
    double now = QpcMs();
    AcquireSRWLockExclusive(&t.lock);
    if (fg != t.fgOn)
    {
        XellLogF("fgmeter %p: frame generation %s, %s", t.sc, fg ? "on" : "off",
                 !fg ? "presents go straight through" : metering ? "pacing" : "measuring only (IGDEXT_FG_METER=1 paces)");
        t.fgOn = fg;
        t.pacer.Reset();
        t.st = Stats();
        t.st.t0 = now;
    }
    if (!fg) { ReleaseSRWLockExclusive(&t.lock); return; }
    const FgPacer::Plan p = t.pacer.Schedule(now);
    ReleaseSRWLockExclusive(&t.lock);

    double out = now;
    if (metering && p.paced)
    {
        WaitUntilMs(p.targetMs);
        out = QpcMs();
    }

    AcquireSRWLockExclusive(&t.lock);
    t.pacer.Presented(metering ? out : now);
    Stats& s = t.st;
    const double ideal = p.perReal > 0 ? p.intervalMs / p.perReal : 0;
    if (s.n && now - s.lastIn < FgPacer::kGapMs)
    {
        const double di = now - s.lastIn, dout = out - s.lastOut;
        s.sumIn += di; s.sumIn2 += di * di; s.maxIn = std::max(s.maxIn, di);
        s.sumOut += dout; s.sumOut2 += dout * dout; s.maxOut = std::max(s.maxOut, dout);
        if (ideal > 0) { s.shortIn += di < 0.5 * ideal; s.shortOut += dout < 0.5 * ideal; }
        ++s.nIv;
    }
    ++s.n; s.lastIn = now; s.lastOut = out;
    if (p.intervalMs > 0) { s.sumSpacing += p.spacingMs; s.sumInterval += p.intervalMs; s.sumPerReal += p.perReal; ++s.nPlan; }
    if (metering && p.paced)
    {
        const double w = out - now;
        ++s.waited; s.sumWait += w; s.maxWait = std::max(s.maxWait, w);
        s.late += out - p.targetMs > 0.5;
    }
    s.capped += p.capped;
    if (XellLogOn() && out - s.t0 >= 2000) Summary(t, out, metering);
    ReleaseSRWLockExclusive(&t.lock);
}

thread_local int t_inPresent = 0;   // DXVK's Present calls its Present1: one present, metered once

HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain* self, UINT sync, UINT flags)
{
    const ScVtbl* v = FindSc(self);
    if (!v || !v->present) return DXGI_ERROR_INVALID_CALL;
    if (t_inPresent || (flags & DXGI_PRESENT_TEST)) return v->present(self, sync, flags);
    ++t_inPresent;
    if (Target* t = FindTarget(self)) Meter(*t);
    const HRESULT hr = v->present(self, sync, flags);
    --t_inPresent;
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_Present1(IDXGISwapChain1* self, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp)
{
    const ScVtbl* v = FindSc(self);
    if (!v || !v->present1) return DXGI_ERROR_INVALID_CALL;
    if (t_inPresent || (flags & DXGI_PRESENT_TEST)) return v->present1(self, sync, flags, pp);
    ++t_inPresent;
    if (Target* t = FindTarget(self)) Meter(*t);
    const HRESULT hr = v->present1(self, sync, flags, pp);
    --t_inPresent;
    return hr;
}

// the Present slots of this object's vtable (Present1 only on an IDXGISwapChain1 pointer)
void HookSwapChainVtbl(void* obj, bool hasPresent1)
{
    void** vtbl = *(void***) obj;
    AcquireSRWLockExclusive(&g_patchLock);
    if (!PinSelf()) { ReleaseSRWLockExclusive(&g_patchLock); XellLogF("fgmeter: could not pin igdext64, nothing hooked"); return; }
    LONG i = 0;
    while (i < g_nSc && g_sc[i].vtbl != vtbl) ++i;
    // a vtable already pointing at the hook is a copy of a hooked one (another hook's): FindSc's fallback serves it
    if (i == g_nSc && i < (LONG) (sizeof(g_sc) / sizeof(g_sc[0])) && vtbl[kSlotPresent] != (void*) Hook_Present)
    {
        // the originals are in the table before the slots point at the hooks
        g_sc[i].vtbl = vtbl;
        g_sc[i].present = (PFN_Present) vtbl[kSlotPresent];
        g_sc[i].present1 = nullptr;
        MemoryBarrier();
        InterlockedIncrement(&g_nSc);
        PatchSlot(vtbl, kSlotPresent, (void*) Hook_Present);
        XellLogF("fgmeter: swap chain vtable %p hooked (Present)", vtbl);
    }
    if (i < g_nSc && hasPresent1 && !g_sc[i].present1 && vtbl[kSlotPresent1] != (void*) Hook_Present1)
    {
        g_sc[i].present1 = (PFN_Present1) vtbl[kSlotPresent1];
        MemoryBarrier();
        PatchSlot(vtbl, kSlotPresent1, (void*) Hook_Present1);
        XellLogF("fgmeter: swap chain vtable %p hooked (Present1)", vtbl);
    }
    ReleaseSRWLockExclusive(&g_patchLock);
}

// both interfaces of a swap chain; returns its IDXGISwapChain1 pointer (no reference kept) or null
void* HookSwapChain(void* sc)
{
    HookSwapChainVtbl(sc, false);
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(((IUnknown*) sc)->QueryInterface(__uuidof(IDXGISwapChain1), (void**) &sc1)) && sc1)
    {
        HookSwapChainVtbl(sc1, true);
        sc1->Release();
    }
    return sc1;
}

// ---- the factory, while XeSS-FG initializes: the swap chain it creates ---------------------------------------------
struct FacVtbl { void** vtbl; PFN_CreateSwapChain cs; PFN_CreateForHwnd hwnd; PFN_CreateForComposition comp; };
FacVtbl g_fac[4];
volatile LONG g_nFac = 0;
volatile LONG g_initActive = 0;
void* volatile g_adopted = nullptr;   // the last swap chain created during the init call
void* g_initApp = nullptr; void* g_initApp1 = nullptr;

const FacVtbl* FindFac(void* obj)
{
    void** vtbl = *(void***) obj;
    const LONG n = g_nFac;
    for (LONG i = 0; i < n; ++i) if (g_fac[i].vtbl == vtbl) return &g_fac[i];
    return n ? &g_fac[0] : nullptr;
}

void Adopt(void* sc)
{
    if (g_initActive && sc) g_adopted = sc;
}

HRESULT STDMETHODCALLTYPE Hook_CreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** out)
{
    const FacVtbl* v = FindFac(f);
    if (!v || !v->cs) return DXGI_ERROR_INVALID_CALL;
    const HRESULT hr = v->cs(f, dev, d, out);
    if (SUCCEEDED(hr) && out) Adopt(*out);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND w, const DXGI_SWAP_CHAIN_DESC1* d,
                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o, IDXGISwapChain1** out)
{
    const FacVtbl* v = FindFac(f);
    if (!v || !v->hwnd) return DXGI_ERROR_INVALID_CALL;
    const HRESULT hr = v->hwnd(f, dev, w, d, fs, o, out);
    if (SUCCEEDED(hr) && out) Adopt(*out);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateForComposition(IDXGIFactory2* f, IUnknown* dev, const DXGI_SWAP_CHAIN_DESC1* d,
                                                    IDXGIOutput* o, IDXGISwapChain1** out)
{
    const FacVtbl* v = FindFac(f);
    if (!v || !v->comp) return DXGI_ERROR_INVALID_CALL;
    const HRESULT hr = v->comp(f, dev, d, o, out);
    if (SUCCEEDED(hr) && out) Adopt(*out);
    return hr;
}

void HookFactory(IDXGIFactory2* f)
{
    void** vtbl = *(void***) f;
    AcquireSRWLockExclusive(&g_patchLock);
    if (!PinSelf()) { ReleaseSRWLockExclusive(&g_patchLock); XellLogF("fgmeter: could not pin igdext64, nothing hooked"); return; }
    LONG i = 0;
    while (i < g_nFac && g_fac[i].vtbl != vtbl) ++i;
    if (i == g_nFac && i < (LONG) (sizeof(g_fac) / sizeof(g_fac[0])) && vtbl[kSlotCreateForHwnd] != (void*) Hook_CreateForHwnd)
    {
        FacVtbl& e = g_fac[i];
        e.vtbl = vtbl;
        e.cs = vtbl[kSlotCreateSwapChain] == (void*) Hook_CreateSwapChain ? nullptr : (PFN_CreateSwapChain) vtbl[kSlotCreateSwapChain];
        e.hwnd = vtbl[kSlotCreateForHwnd] == (void*) Hook_CreateForHwnd ? nullptr : (PFN_CreateForHwnd) vtbl[kSlotCreateForHwnd];
        e.comp = vtbl[kSlotCreateForComposition] == (void*) Hook_CreateForComposition ? nullptr : (PFN_CreateForComposition) vtbl[kSlotCreateForComposition];
        MemoryBarrier();
        InterlockedIncrement(&g_nFac);
        if (e.cs) PatchSlot(vtbl, kSlotCreateSwapChain, (void*) Hook_CreateSwapChain);
        if (e.hwnd) PatchSlot(vtbl, kSlotCreateForHwnd, (void*) Hook_CreateForHwnd);
        if (e.comp) PatchSlot(vtbl, kSlotCreateForComposition, (void*) Hook_CreateForComposition);
        XellLogF("fgmeter: DXGI factory vtable %p hooked", vtbl);
    }
    ReleaseSRWLockExclusive(&g_patchLock);
}
}

bool FgMeterOn()
{
    if (g_mode < 0)
    {
        char b[16] = {};
        const DWORD n = GetEnvironmentVariableA("IGDEXT_FG_METER", b, sizeof(b));
        g_mode = n > 0 && n < sizeof(b) && !strcmp(b, "1") ? 1 : 0;
        if (n > 0 && strcmp(b, "1") && strcmp(b, "0")) XellLogF("fgmeter: IGDEXT_FG_METER=%s is not 0 or 1: off", b);
    }
    return g_mode == 1;
}

bool FgMeterWanted()
{
    return FgMeterOn() || XellLogOn();
}

void FgMeterInitBegin(void* appSwapChain, void* factory)
{
    if (!FgMeterWanted()) return;
    IDXGIFactory2* f2 = nullptr;
    if (factory) ((IUnknown*) factory)->QueryInterface(__uuidof(IDXGIFactory2), (void**) &f2);
    else if (appSwapChain) ((IDXGISwapChain*) appSwapChain)->GetParent(__uuidof(IDXGIFactory2), (void**) &f2);
    if (f2) { HookFactory(f2); f2->Release(); }
    else XellLogF("fgmeter: no DXGI factory for XeSS-FG's initialization, its swap chain may not be found");
    // the game's swap chain is still alive here: hooked now, used if XeSS-FG creates none (it may release it in the call)
    g_initApp = appSwapChain;
    g_initApp1 = appSwapChain ? HookSwapChain(appSwapChain) : nullptr;
    g_adopted = nullptr;
    InterlockedExchange(&g_initActive, 1);
}

void FgMeterInitEnd(void* ctx, int32_t result)
{
    if (!FgMeterWanted()) return;
    InterlockedExchange(&g_initActive, 0);
    void* sc = g_adopted; void* sc1 = nullptr;
    const char* how = "created by XeSS-FG";
    if (result != 0) { XellLogF("fgmeter: XeSS-FG initialization failed (%d), nothing paced", result); return; }
    if (sc) sc1 = HookSwapChain(sc);
    else if (g_initApp) { sc = g_initApp; sc1 = g_initApp1; how = "the game's, XeSS-FG created none"; }
    else { XellLogF("fgmeter: ctx %p: no swap chain seen during XeSS-FG's initialization, nothing paced", ctx); return; }
    AcquireSRWLockExclusive(&g_patchLock);
    Target* t = nullptr;
    for (Target& e : g_targets) if (e.ctx == ctx || (!t && !e.sc)) t = &e;
    if (!t) t = &g_targets[g_nextTarget++ % 4];
    AcquireSRWLockExclusive(&t->lock);
    t->sc = nullptr; t->sc1 = nullptr;
    MemoryBarrier();
    t->ctx = ctx; t->pacer.Reset(); t->fgOn = -1; t->st = Stats();
    const LONG noted = g_noted;
    t->pacer.NoteGenerated(noted);
    t->sc1 = sc1 != sc ? sc1 : nullptr;
    t->sc = sc;
    ReleaseSRWLockExclusive(&t->lock);
    ReleaseSRWLockExclusive(&g_patchLock);
    XellLogF("fgmeter: ctx %p: swap chain %p (%s) %s", ctx, sc, how,
             FgMeterOn() ? "is paced while frame generation is on (IGDEXT_FG_METER=1)" : "is measured, not paced (IGDEXT_FG_METER=1 paces)");
}

void FgMeterContextDestroyed(void* ctx)
{
    if (!ctx) return;
    AcquireSRWLockExclusive(&g_patchLock);
    for (Target& t : g_targets)
        if (t.ctx == ctx)
        {
            AcquireSRWLockExclusive(&t.lock);
            XellLogF("fgmeter: ctx %p destroyed, swap chain %p no longer paced", ctx, t.sc);
            t.sc = nullptr; t.sc1 = nullptr; t.ctx = nullptr; t.pacer.Reset();
            ReleaseSRWLockExclusive(&t.lock);
        }
    ReleaseSRWLockExclusive(&g_patchLock);
}

void FgMeterRealFrame(void* ctx, uint32_t presentId)
{
    if (!ctx) return;
    for (Target& t : g_targets)
        if (t.ctx == ctx && t.sc)
        {
            const double now = QpcMs();
            AcquireSRWLockExclusive(&t.lock);
            t.pacer.RealFrame(now, presentId);
            ReleaseSRWLockExclusive(&t.lock);
            return;
        }
}

void FgMeterNoteFrames(uint32_t generated, const char* source)
{
    const LONG n = generated > 16 ? 16 : (LONG) generated;
    if (InterlockedExchange(&g_noted, n) != n) XellLogF("fgmeter: %ld generated frame(s) per rendered frame (%s)", (long) n, source);
    for (Target& t : g_targets)
        if (t.sc)
        {
            AcquireSRWLockExclusive(&t.lock);
            t.pacer.NoteGenerated(n);
            ReleaseSRWLockExclusive(&t.lock);
        }
}
