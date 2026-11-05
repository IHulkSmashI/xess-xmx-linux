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
// So the shim spaces them itself, on the swap chain XeSS-FG presents to: the one it creates in
// xefgSwapChainD3D12InitFromSwapChain(Desc), found by hooking DXVK's DXGI factory for the length of that call. Each
// Present is held until the previous one + real-frame interval / presents per real frame (FgPacer.h has the schedule).
// The wait is a short sleep (NtDelayExecution) and a spin of at most 0.5 ms on QPC.
//
// Safety: only DXVK's own classes are patched. A factory or swap chain vtable is hooked only if the vtable and the
// functions in the slots that are patched or called (QueryInterface, Present, Present1, CreateSwapChain*) all lie in the
// system's dxgi.dll (under Proton: DXVK's). A wrapper's vtable (OptiScaler's dxgi.dll proxy wraps the game's swap chain
// and hands that to InitFromSwapChain) is never patched, and nothing is called on it but GetParent, which XeSS-FG calls
// as well. Only the swap chain XeSS-FG itself creates during the init call, on that thread, is paced; if there is none,
// nothing is paced and the log says why. A present on a vtable the meter did not patch (a copy of a patched one) calls
// no original of another class: it fails with DXGI_ERROR_INVALID_CALL, logged once.
//
// Why the wait stays on the presenting thread (no FSR 3 style presenter thread): AMD's FrameInterpolationSwapchainDX12
// owns the whole swap chain - its own back buffers, its own GetCurrentBackBufferIndex, copies on its own present queue,
// and the real Present is called only from its presenter thread. Underneath XeSS-FG's proxy the meter owns none of
// that. Deferring DXVK's Present to another thread would (1) leave GetCurrentBackBufferIndex unadvanced when XeSS-FG
// returns and copies the next frame, so it would write into the buffer still waiting to be shown; (2) order the present
// on the D3D12 queue (vkd3d-proton records it into the queue's timeline at the call) behind the work submitted after
// it - the game's whole next frame - so the frame would be shown a frame later, not paced; (3) race ResizeBuffers,
// SetFullscreenState and Release, which DXGI does not allow concurrently with Present; (4) lose Present's result
// (occluded, device removed) and the frame-latency wait vkd3d-proton does inside Present. So the present is held where
// it is, and the hold is made visible to XeLL instead (FgMeterHeldMs / FgMeterXellFrame): on Windows the driver meters
// the flips without holding the thread, so XeLL's present-end marker is not moved; here the low latency of
// XellHook.cpp takes the hold off the present end it schedules from, so the next frame starts when it would have
// without the meter and the hold replaces part of xellSleep's wait instead of adding to it. And a hold never goes
// beyond what the next frame can absorb (the low latency's slack): pacing gives way before latency does. The 2 s
// summary logs, per real frame, the hold inside the game's present, and the part of it that was not absorbed (added).
//
// IGDEXT_FG_METER=1 paces, =measure only measures, 0 or unset installs nothing (whatever IGDEXT_XELL_LOG says; ClawDeck
// sets that by default). With IGDEXT_XELL_LOG=1 C:\igdext_xell.log gets a summary every 2 s ("fgmeter: ...").
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
int g_mode = -1;   // -1 not read, 0 off, 1 pace, 2 measure

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
    kSlotQueryInterface = 0,
    kSlotPresent = 8,                  // IDXGISwapChain: IDXGIDeviceSubObject::GetDevice 7, Present 8
    kSlotPresent1 = 22,                // IDXGISwapChain1: GetDesc1 18 ... GetCoreWindow 21, Present1 22
    kSlotCreateSwapChain = 10,         // IDXGIFactory: EnumAdapters 7 ... CreateSwapChain 10
    kSlotCreateForHwnd = 15,           // IDXGIFactory2: IsWindowedStereoEnabled 14, CreateSwapChainForHwnd 15
    kSlotCreateForComposition = 24,    // ... UnregisterOcclusionStatus 23, CreateSwapChainForComposition 24
};

// QPC in ns, as XellHook.cpp's low latency converts it (XeLL's frame reports are on this clock)
uint64_t QpcNs()
{
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (uint64_t) (t.QuadPart / f.QuadPart) * 1000000000ull + (uint64_t) (t.QuadPart % f.QuadPart) * 1000000000ull / (uint64_t) f.QuadPart;
}
double QpcMs() { return (double) QpcNs() / 1e6; }

// ---- the wait ------------------------------------------------------------------------------------------------------
// Until QPC reaches the time: NtDelayExecution (100 ns units, precise under Wine: it is a futex / nanosleep wait) for
// all but the late wake-up seen recently, then a spin of what is left. The late wake-up is tracked as its 90th
// percentile (a stochastic quantile: +9 us when later than the estimate, -1 us when not); measured under Wine on the
// test machine: median 0.10-0.16 ms, p90 0.13-0.22 ms, with rare outliers of 1-6 ms that no spin should chase. The
// spin is kept between 0.05 and 0.5 ms (the hard ceiling: battery first); a wake-up later than that is taken as it is.
const double kMinSpinMs = 0.05, kMaxSpinMs = 0.5;
struct WaitResult { double spunMs; bool slept; };
WaitResult WaitUntilMs(double targetMs)
{
    static PFN_NtDelayExecution delay = (PFN_NtDelayExecution) GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtDelayExecution");
    static thread_local double lateMs = 0.2;
    WaitResult r = { 0, false };
    const double now = QpcMs(), left = targetMs - now;
    if (left <= 0) return r;
    const double spin = std::min(kMaxSpinMs, std::max(kMinSpinMs, lateMs));
    if (left > spin)
    {
        const double ask = left - spin;
        if (delay) { LARGE_INTEGER rel; rel.QuadPart = -(LONGLONG) (ask * 10000.0); delay(FALSE, &rel); }
        else Sleep((DWORD) ask);
        r.slept = true;
        const double late = QpcMs() - now - ask;
        lateMs = std::min(kMaxSpinMs, std::max(kMinSpinMs, lateMs + (late > lateMs ? 0.009 : -0.001)));
    }
    const double from = QpcMs();
    if (from >= targetMs) return r;
    double t = from;
    while (t < targetMs) { YieldProcessor(); t = QpcMs(); }
    r.spunMs = t - from;
    return r;
}

// ---- which classes may be patched: DXVK's -------------------------------------------------------------------------
// the dxgi.dll whose classes are hooked: the system's (under Proton DXVK's, in the prefix's system32). A dxgi.dll proxy
// in the game's folder (OptiScaler) has another path. The Wine test uses a stand-in next to the test executable.
const wchar_t* DxvkDxgiPath()
{
    static wchar_t path[MAX_PATH + 32] = {};
    if (!path[0])
    {
#ifdef FGMETER_TEST_DXGI
        DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        while (n && path[n - 1] != L'\\' && path[n - 1] != L'/') --n;
        path[n] = 0;
        wcscat_s(path, FGMETER_TEST_DXGI);
#else
        const UINT n = GetSystemDirectoryW(path, MAX_PATH);
        if (n && n < MAX_PATH) wcscat_s(path, L"\\dxgi.dll");
        else path[0] = L'?';
#endif
    }
    return path;
}

// true if the address lies in DXVK's dxgi.dll; else `who` names the module it lies in, for the log
bool InDxvkDxgi(const void* addr, char* who, size_t cap)
{
    HMODULE m = nullptr;
    wchar_t path[MAX_PATH] = L"";
    if (!addr || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     (LPCWSTR) addr, &m) || !m || !GetModuleFileNameW(m, path, MAX_PATH))
    {
        snprintf(who, cap, "no module (heap or generated code)");
        return false;
    }
    if (!_wcsicmp(path, DxvkDxgiPath())) return true;
    snprintf(who, cap, "%ls", path);
    return false;
}

// the vtable itself and the functions in these slots are DXVK's
bool VtblIsDxvk(void** vtbl, const int* slots, int n, const char* what)
{
    char who[MAX_PATH + 64];
    if (!InDxvkDxgi(vtbl, who, sizeof(who)))
    {
        XellLogF("fgmeter: %s vtable %p lies in %s, not DXVK's %ls: not hooked", what, vtbl, who, DxvkDxgiPath());
        return false;
    }
    for (int i = 0; i < n; ++i)
    {
        if (!InDxvkDxgi(vtbl[slots[i]], who, sizeof(who)))
        {
            XellLogF("fgmeter: %s vtable %p slot %d points into %s, not DXVK's %ls (another hook): not hooked", what, vtbl,
                     slots[i], who, DxvkDxgiPath());
            return false;
        }
    }
    return true;
}

// a vtable slot, written in place; the page keeps its execute right if it had one (a vtable next to code)
bool PatchSlot(void** vtbl, int slot, void* hook)
{
    if (vtbl[slot] == hook) return true;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(&vtbl[slot], &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    DWORD prot;
    if (!VirtualProtect(&vtbl[slot], sizeof(void*), (mbi.Protect & exec) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &prot)) return false;
    InterlockedExchangePointer(&vtbl[slot], hook);
    VirtualProtect(&vtbl[slot], sizeof(void*), prot, &prot);
    return true;
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
    return nullptr;
}

// a present through a vtable the meter did not patch (one copied from a patched one by another hook): its own original
// is not known, and another class's is never called
HRESULT UnknownVtbl(void* obj, const char* what)
{
    static volatile LONG said = 0;
    if (InterlockedExchange(&said, 1) == 0)
        XellLogF("fgmeter: %s on vtable %p, which the meter did not patch (a copy of a hooked one?): DXGI_ERROR_INVALID_CALL", what, *(void***) obj);
    return DXGI_ERROR_INVALID_CALL;
}

// ---- the XeLL coupling: the holds, and what the low latency says about them -----------------------------------------
struct HoldRec { uint64_t fromNs, toNs; };
HoldRec g_hold[512];
LONG g_nHold = 0;
SRWLOCK g_holdLock = SRWLOCK_INIT;

void AddHold(uint64_t fromNs, uint64_t toNs)
{
    AcquireSRWLockExclusive(&g_holdLock);
    g_hold[g_nHold % 512] = { fromNs, toNs };
    ++g_nHold;
    ReleaseSRWLockExclusive(&g_holdLock);
}

struct XellStats { int n = 0, added = 0; double sumHeld = 0, maxHeld = 0, sumAdded = 0, maxAdded = 0, sumSlack = 0; };
SRWLOCK g_xellLock = SRWLOCK_INIT;
XellStats g_xs;
double g_xSlack[4] = {}, g_xHeld[8] = {};
int g_xN = 0;
double g_xLastMs = 0;
int g_xSaid = 0;

// how long the holds of the frame being presented may last in all, or -1: no limit known (no low latency reports in
// the last 0.5 s, or the holds are not inside the game's present - XeSS-FG presenting on a thread of its own - so
// they cannot delay the next frame)
double AbsorbableMs(double nowMs)
{
    AcquireSRWLockShared(&g_xellLock);
    double r = -1;
    if (g_xN >= 4 && nowMs - g_xLastMs < 500)
    {
        bool onPath = false;
        for (int i = 0; i < 8 && i < g_xN; ++i) onPath |= g_xHeld[i] > 0.5;
        if (onPath)
        {
            r = 1e9;
            for (double s : g_xSlack) r = std::min(r, s);
            r = std::max(0.0, r - 0.3);   // the low latency's own timing varies by a few tenths
        }
    }
    ReleaseSRWLockShared(&g_xellLock);
    return r;
}

// ---- what is paced -----------------------------------------------------------------------------------------------
struct Stats
{
    double t0 = 0, lastIn = 0, lastOut = 0;
    int n = 0, nIv = 0, shortIn = 0, shortOut = 0, waited = 0, capped = 0, xellCapped = 0, late = 0, slept = 0;
    double sumIn = 0, sumIn2 = 0, maxIn = 0, sumOut = 0, sumOut2 = 0, maxOut = 0, sumWait = 0, maxWait = 0;
    double sumSpin = 0, maxSpin = 0;
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
    double heldThisFrame = 0;   // the holds since the last rendered frame's mark
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

void Summary(Target& t, double now, bool pacing)
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
             " | waited %d x avg %.2f max %.2f ms, capped %d (interval) %d (xell slack), late %d"
             " | spin per present avg %.3f max %.3f ms, %d sleeps",
             t.sc, pacing ? "meter on" : "measuring only (IGDEXT_FG_METER=measure)", secs, s.n, s.n / (secs > 0 ? secs : 1),
             s.sumInterval / np, s.sumPerReal / np, s.sumSpacing / np,
             inAvg, inSd, s.maxIn, 100 * s.shortIn / iv, outAvg, outSd, s.maxOut, 100 * s.shortOut / iv,
             s.waited, s.waited ? s.sumWait / s.waited : 0.0, s.maxWait, s.capped, s.xellCapped, s.late,
             s.n ? s.sumSpin / s.n : 0.0, s.maxSpin, s.slept);
    AcquireSRWLockExclusive(&g_xellLock);
    const XellStats x = g_xs;
    g_xs = XellStats();
    ReleaseSRWLockExclusive(&g_xellLock);
    if (x.n)
        XellLogF("fgmeter %p: latency, %d real frames: held in the game's present avg %.2f max %.2f ms, absorbed by xellSleep"
                 " (slack avg %.2f ms); added latency per real frame avg %.3f max %.3f ms (%d frames over 0.1 ms)",
                 t.sc, x.n, x.sumHeld / x.n, x.maxHeld, x.sumSlack / x.n, x.sumAdded / x.n, x.maxAdded, x.added);
    else if (pacing)
        XellLogF("fgmeter %p: latency: no low latency frames (xellSleep not called, or IGDEXT_XELL_CAP not auto): the hold is"
                 " not absorbed and the added latency is not known; at most the hold above", t.sc);
    s = Stats();
    s.t0 = now;
}

// before the real Present of a target: wait for its turn (pacing on), and count
void Meter(Target& t)
{
    const bool pacing = FgMeterOn();
    const int fg = XefgFrameGenerationState() == 1;
    const uint64_t nowNs = QpcNs();
    const double now = (double) nowNs / 1e6;
    AcquireSRWLockExclusive(&t.lock);
    if (fg != t.fgOn)
    {
        XellLogF("fgmeter %p: frame generation %s, %s", t.sc, fg ? "on" : "off",
                 !fg ? "presents go straight through" : pacing ? "pacing" : "measuring only (IGDEXT_FG_METER=1 paces)");
        t.fgOn = fg;
        t.pacer.Reset();
        t.st = Stats();
        t.st.t0 = now;
    }
    if (!fg) { ReleaseSRWLockExclusive(&t.lock); return; }
    FgPacer::Plan p = t.pacer.Schedule(now);
    bool xellCapped = false;
    if (pacing && p.paced)
    {
        // never more hold in one frame's present than the next frame can absorb (the low latency's slack)
        const double allow = AbsorbableMs(now);
        if (allow >= 0)
        {
            const double room = allow - t.heldThisFrame;
            if (p.targetMs - now > room)
            {
                xellCapped = true;
                if (room < 0.05) p.paced = false;
                else p.targetMs = now + room;
            }
        }
    }
    ReleaseSRWLockExclusive(&t.lock);

    double out = now;
    WaitResult w = { 0, false };
    if (pacing && p.paced)
    {
        w = WaitUntilMs(p.targetMs);
        const uint64_t outNs = QpcNs();
        out = (double) outNs / 1e6;
        AddHold(nowNs, outNs);
    }

    AcquireSRWLockExclusive(&t.lock);
    t.pacer.Presented(pacing ? out : now);
    t.heldThisFrame += out - now;
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
    if (pacing && p.paced)
    {
        const double wt = out - now;
        ++s.waited; s.sumWait += wt; s.maxWait = std::max(s.maxWait, wt);
        s.late += out - p.targetMs > 0.5;
        s.sumSpin += w.spunMs; s.maxSpin = std::max(s.maxSpin, w.spunMs); s.slept += w.slept;
    }
    s.capped += p.capped;
    s.xellCapped += xellCapped;
    if (XellLogOn() && out - s.t0 >= 2000) Summary(t, out, pacing);
    ReleaseSRWLockExclusive(&t.lock);
}

thread_local int t_inPresent = 0;   // DXVK's Present calls its Present1: one present, metered once

HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain* self, UINT sync, UINT flags)
{
    const ScVtbl* v = FindSc(self);
    if (!v || !v->present) return UnknownVtbl(self, "Present");
    Target* t = nullptr;
    if (t_inPresent || (flags & DXGI_PRESENT_TEST) || !(t = FindTarget(self))) return v->present(self, sync, flags);
    ++t_inPresent;
    Meter(*t);
    const HRESULT hr = v->present(self, sync, flags);
    --t_inPresent;
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_Present1(IDXGISwapChain1* self, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp)
{
    const ScVtbl* v = FindSc(self);
    if (!v || !v->present1) return UnknownVtbl(self, "Present1");
    Target* t = nullptr;
    if (t_inPresent || (flags & DXGI_PRESENT_TEST) || !(t = FindTarget(self))) return v->present1(self, sync, flags, pp);
    ++t_inPresent;
    Meter(*t);
    const HRESULT hr = v->present1(self, sync, flags, pp);
    --t_inPresent;
    return hr;
}

// the Present slot (and Present1 on an IDXGISwapChain1 pointer) of a DXVK swap chain vtable; false if it is not DXVK's
bool HookSwapChainVtbl(void** vtbl, bool present1)
{
    AcquireSRWLockExclusive(&g_patchLock);
    LONG i = 0;
    while (i < g_nSc && g_sc[i].vtbl != vtbl) ++i;
    bool ok = true;
    if (i == g_nSc)
    {
        const int slots[] = { kSlotQueryInterface, kSlotPresent };
        if (vtbl[kSlotPresent] == (void*) Hook_Present)
        {
            XellLogF("fgmeter: swap chain vtable %p already points at the meter but is not one it patched (a copy): not hooked", vtbl);
            ok = false;
        }
        else if (i >= (LONG) (sizeof(g_sc) / sizeof(g_sc[0])) || !VtblIsDxvk(vtbl, slots, 2, "swap chain") || !PinSelf())
            ok = false;
        else
        {
            // the originals are in the table before the slot points at the hook
            g_sc[i].vtbl = vtbl;
            g_sc[i].present = (PFN_Present) vtbl[kSlotPresent];
            g_sc[i].present1 = nullptr;
            MemoryBarrier();
            InterlockedIncrement(&g_nSc);
            if (PatchSlot(vtbl, kSlotPresent, (void*) Hook_Present)) XellLogF("fgmeter: DXVK swap chain vtable %p hooked (Present)", vtbl);
            else { XellLogF("fgmeter: swap chain vtable %p: slot not writable, not hooked", vtbl); ok = false; }
        }
    }
    if (ok && present1 && !g_sc[i].present1)
    {
        const int slots[] = { kSlotPresent1 };
        if (vtbl[kSlotPresent1] != (void*) Hook_Present1 && VtblIsDxvk(vtbl, slots, 1, "swap chain"))
        {
            g_sc[i].present1 = (PFN_Present1) vtbl[kSlotPresent1];
            MemoryBarrier();
            if (PatchSlot(vtbl, kSlotPresent1, (void*) Hook_Present1)) XellLogF("fgmeter: DXVK swap chain vtable %p hooked (Present1)", vtbl);
            else XellLogF("fgmeter: swap chain vtable %p: Present1 slot not writable, not hooked", vtbl);
        }
    }
    ReleaseSRWLockExclusive(&g_patchLock);
    return ok;
}

// a swap chain XeSS-FG created: both interfaces, if it is DXVK's; sc1 gets its IDXGISwapChain1 pointer (no reference)
bool HookSwapChain(void* sc, void** sc1Out)
{
    *sc1Out = nullptr;
    if (!HookSwapChainVtbl(*(void***) sc, false)) return false;
    // QueryInterface is DXVK's (checked above)
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(((IUnknown*) sc)->QueryInterface(__uuidof(IDXGISwapChain1), (void**) &sc1)) && sc1)
    {
        if (HookSwapChainVtbl(*(void***) sc1, true)) *sc1Out = sc1;
        sc1->Release();
    }
    return true;
}

// ---- the factory, while XeSS-FG initializes: the swap chain it creates ---------------------------------------------
struct FacVtbl { void** vtbl; PFN_CreateSwapChain cs; PFN_CreateForHwnd hwnd; PFN_CreateForComposition comp; };
FacVtbl g_fac[4];
volatile LONG g_nFac = 0;
volatile LONG g_initActive = 0;
volatile DWORD g_initThread = 0;
void* volatile g_adopted = nullptr;   // the last swap chain created on the init thread during the init call
volatile LONG g_adoptedElsewhere = 0;
bool g_initFactoryHooked = false;

const FacVtbl* FindFac(void* obj)
{
    void** vtbl = *(void***) obj;
    const LONG n = g_nFac;
    for (LONG i = 0; i < n; ++i) if (g_fac[i].vtbl == vtbl) return &g_fac[i];
    return nullptr;
}

void Adopt(void* sc)
{
    if (!g_initActive || !sc) return;
    if (GetCurrentThreadId() == g_initThread) g_adopted = sc;
    else InterlockedIncrement(&g_adoptedElsewhere);
}

HRESULT STDMETHODCALLTYPE Hook_CreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** out)
{
    const FacVtbl* v = FindFac(f);
    if (!v || !v->cs) return UnknownVtbl(f, "CreateSwapChain");
    const HRESULT hr = v->cs(f, dev, d, out);
    if (SUCCEEDED(hr) && out) Adopt(*out);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND w, const DXGI_SWAP_CHAIN_DESC1* d,
                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o, IDXGISwapChain1** out)
{
    const FacVtbl* v = FindFac(f);
    if (!v || !v->hwnd) return UnknownVtbl(f, "CreateSwapChainForHwnd");
    const HRESULT hr = v->hwnd(f, dev, w, d, fs, o, out);
    if (SUCCEEDED(hr) && out) Adopt(*out);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateForComposition(IDXGIFactory2* f, IUnknown* dev, const DXGI_SWAP_CHAIN_DESC1* d,
                                                    IDXGIOutput* o, IDXGISwapChain1** out)
{
    const FacVtbl* v = FindFac(f);
    if (!v || !v->comp) return UnknownVtbl(f, "CreateSwapChainForComposition");
    const HRESULT hr = v->comp(f, dev, d, o, out);
    if (SUCCEEDED(hr) && out) Adopt(*out);
    return hr;
}

// DXVK's factory class: its three swap chain creators; false if it is not DXVK's
bool HookFactory(IDXGIFactory2* f)
{
    void** vtbl = *(void***) f;
    AcquireSRWLockExclusive(&g_patchLock);
    LONG i = 0;
    while (i < g_nFac && g_fac[i].vtbl != vtbl) ++i;
    bool ok = i < g_nFac;
    if (!ok)
    {
        const int slots[] = { kSlotCreateSwapChain, kSlotCreateForHwnd, kSlotCreateForComposition };
        if (vtbl[kSlotCreateForHwnd] == (void*) Hook_CreateForHwnd)
            XellLogF("fgmeter: DXGI factory vtable %p already points at the meter but is not one it patched (a copy): not hooked", vtbl);
        else if (i < (LONG) (sizeof(g_fac) / sizeof(g_fac[0])) && VtblIsDxvk(vtbl, slots, 3, "DXGI factory") && PinSelf())
        {
            FacVtbl& e = g_fac[i];
            e.vtbl = vtbl;
            e.cs = (PFN_CreateSwapChain) vtbl[kSlotCreateSwapChain];
            e.hwnd = (PFN_CreateForHwnd) vtbl[kSlotCreateForHwnd];
            e.comp = (PFN_CreateForComposition) vtbl[kSlotCreateForComposition];
            MemoryBarrier();
            InterlockedIncrement(&g_nFac);
            ok = PatchSlot(vtbl, kSlotCreateSwapChain, (void*) Hook_CreateSwapChain);
            ok = PatchSlot(vtbl, kSlotCreateForHwnd, (void*) Hook_CreateForHwnd) && ok;
            ok = PatchSlot(vtbl, kSlotCreateForComposition, (void*) Hook_CreateForComposition) && ok;
            XellLogF("fgmeter: DXVK DXGI factory vtable %p %s", vtbl, ok ? "hooked" : "only partly hooked (slot not writable)");
        }
    }
    ReleaseSRWLockExclusive(&g_patchLock);
    return ok;
}
}

bool FgMeterOn()
{
    if (g_mode < 0)
    {
        char b[16] = {};
        const DWORD n = GetEnvironmentVariableA("IGDEXT_FG_METER", b, sizeof(b));
        const bool set = n > 0 && n < sizeof(b);
        g_mode = set && !strcmp(b, "1") ? 1 : set && !_stricmp(b, "measure") ? 2 : 0;
        if (n > 0 && !g_mode && strcmp(b, "0")) XellLogF("fgmeter: IGDEXT_FG_METER=%s is not 1, measure or 0: off", set ? b : "(too long)");
    }
    return g_mode == 1;
}

bool FgMeterWanted()
{
    FgMeterOn();
    return g_mode > 0;
}

void FgMeterInitBegin(void* appSwapChain, void* factory)
{
    if (!FgMeterWanted()) return;
    // the factory XeSS-FG will create its swap chain with: the one it is given, or the game swap chain's parent (a
    // wrapper's GetParent is the only call made on it, and XeSS-FG makes it as well)
    IDXGIFactory2* f2 = nullptr;
    if (factory) ((IUnknown*) factory)->QueryInterface(__uuidof(IDXGIFactory2), (void**) &f2);
    else if (appSwapChain) ((IDXGISwapChain*) appSwapChain)->GetParent(__uuidof(IDXGIFactory2), (void**) &f2);
    g_initFactoryHooked = f2 && HookFactory(f2);
    if (!f2) XellLogF("fgmeter: no DXGI factory for XeSS-FG's initialization");
    if (f2) f2->Release();
    if (appSwapChain) XellLogF("fgmeter: the game's swap chain %p (vtable %p) is left alone; only the one XeSS-FG creates is paced",
                               appSwapChain, *(void**) appSwapChain);
    g_adopted = nullptr;
    g_adoptedElsewhere = 0;
    g_initThread = GetCurrentThreadId();
    InterlockedExchange(&g_initActive, 1);
}

void FgMeterInitEnd(void* ctx, int32_t result)
{
    if (!FgMeterWanted()) return;
    InterlockedExchange(&g_initActive, 0);
    void* sc = g_adopted;
    if (result != 0) { XellLogF("fgmeter: ctx %p: XeSS-FG initialization failed (%d), nothing paced", ctx, result); return; }
    if (!sc)
    {
        XellLogF("fgmeter: ctx %p: nothing paced: %s", ctx,
                 !g_initFactoryHooked ? "the factory is not DXVK's (see above), so XeSS-FG's swap chain cannot be seen"
                 : g_adoptedElsewhere ? "XeSS-FG's swap chain was created on another thread" : "XeSS-FG created no swap chain during its initialization");
        return;
    }
    void* sc1 = nullptr;
    if (!HookSwapChain(sc, &sc1))
    {
        XellLogF("fgmeter: ctx %p: XeSS-FG's swap chain %p is not DXVK's (see above), nothing paced", ctx, sc);
        return;
    }
    AcquireSRWLockExclusive(&g_patchLock);
    Target* t = nullptr;
    for (Target& e : g_targets) if (e.ctx == ctx || (!t && !e.sc)) t = &e;
    if (!t) t = &g_targets[g_nextTarget++ % 4];
    AcquireSRWLockExclusive(&t->lock);
    t->sc = nullptr; t->sc1 = nullptr;
    MemoryBarrier();
    t->ctx = ctx; t->pacer.Reset(); t->fgOn = -1; t->st = Stats(); t->heldThisFrame = 0;
    const LONG noted = g_noted;
    t->pacer.NoteGenerated(noted);
    t->sc1 = sc1 != sc ? sc1 : nullptr;
    t->sc = sc;
    ReleaseSRWLockExclusive(&t->lock);
    ReleaseSRWLockExclusive(&g_patchLock);
    XellLogF("fgmeter: ctx %p: swap chain %p (created by XeSS-FG, DXVK's) %s", ctx, sc,
             FgMeterOn() ? "is paced while frame generation is on (IGDEXT_FG_METER=1)" : "is measured, not paced (IGDEXT_FG_METER=measure)");
}

void FgMeterContextDestroyed(void* ctx)
{
    if (!ctx || !FgMeterWanted()) return;
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
    if (!ctx || !FgMeterWanted()) return;
    for (Target& t : g_targets)
        if (t.ctx == ctx && t.sc)
        {
            const double now = QpcMs();
            AcquireSRWLockExclusive(&t.lock);
            t.pacer.RealFrame(now, presentId);
            t.heldThisFrame = 0;
            ReleaseSRWLockExclusive(&t.lock);
            return;
        }
}

void FgMeterNoteFrames(uint32_t generated, const char* source)
{
    if (!FgMeterWanted()) return;
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

double FgMeterHeldMs(uint64_t fromNs, uint64_t toNs)
{
    if (g_mode != 1 || !fromNs || toNs <= fromNs) return 0;
    double ns = 0;
    AcquireSRWLockShared(&g_holdLock);
    const LONG n = g_nHold < 512 ? g_nHold : 512;
    for (LONG k = 0; k < n; ++k)
    {
        const HoldRec& h = g_hold[(g_nHold - 1 - k) % 512];
        if (h.toNs <= fromNs) { if (h.toNs + 1000000000ull < fromNs) break; continue; }   // a second older: done
        const uint64_t a = h.fromNs > fromNs ? h.fromNs : fromNs, b = h.toNs < toNs ? h.toNs : toNs;
        if (b > a) ns += (double) (b - a);
    }
    ReleaseSRWLockShared(&g_holdLock);
    return std::min(ns, (double) (toNs - fromNs)) / 1e6;
}

void FgMeterXellFrame(double heldMs, double leftMs, double slackMs)
{
    if (g_mode != 1) return;
    // the hold made the next frame late only by what the low latency could not take off its wait
    const double added = std::min(heldMs, std::max(0.0, -leftMs));
    AcquireSRWLockExclusive(&g_xellLock);
    g_xSlack[g_xN % 4] = slackMs;
    g_xHeld[g_xN % 8] = heldMs;
    ++g_xN;
    g_xLastMs = QpcMs();
    XellStats& x = g_xs;
    ++x.n; x.sumHeld += heldMs; x.maxHeld = std::max(x.maxHeld, heldMs); x.sumSlack += slackMs;
    x.sumAdded += added; x.maxAdded = std::max(x.maxAdded, added); x.added += added > 0.1;
    const bool say = heldMs > 0 && g_xSaid < 8;
    if (say) ++g_xSaid;
    ReleaseSRWLockExclusive(&g_xellLock);
    if (say) XellLogF("fgmeter: real frame: held %.2f ms in the game's present, low latency wait %.2f ms after taking it off,"
                      " slack %.2f ms, added latency %.3f ms", heldMs, leftMs, slackMs, added);
}
