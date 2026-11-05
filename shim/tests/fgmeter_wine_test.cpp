// fgmeter_wine_test.cpp - FgMeter.cpp end to end, on fake DXGI objects, in real time (Windows or Wine). Build (any d3d12.h
// with the raytracing types: mingw-w64's own is too old for igdext.h):
//   x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -shared shim/tests/fgmeter_fake_dxgi.cpp -o fgmeter_fake_dxgi.dll
//   x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -DNOMINMAX -DWIN32_LEAN_AND_MEAN -DINTC_IGDEXT_D3D11 -DINTC_IGDEXT_D3D12
//     '-DFGMETER_TEST_DXGI=L"fgmeter_fake_dxgi.dll"' -I<DirectX-Headers>/include/directx -Ishim/src -Ishim/src/legacy
//     -Ishim/d3dext-sdk/include/public shim/tests/fgmeter_wine_test.cpp shim/src/dll/FgMeter.cpp -o fgmeter_wine_test.exe
//   wine fgmeter_wine_test.exe          (both files in one folder; runs every case below in a child process of its own)
//
// fgmeter_fake_dxgi.dll stands in for DXVK's dxgi.dll: its factory and swap chain vtables and functions lie in that
// module. An OptiScaler-style wrapper swap chain lives in this executable (outside that module) and forwards to an
// inner DXVK one; it is what the game hands to xefgSwapChainD3D12InitFromSwapChain, as OptiScaler does. Inside the init
// call "XeSS-FG" creates its own swap chain through the DXVK factory. Then the game loop, per rendered frame: the low
// latency's wait (XellHook.cpp's AutoPaceWait reduced to one frame in flight: the next frame starts at the present end
// + the frame time - the CPU time, with the meter's hold taken off the present end), the CPU time, the present through
// the wrapper, and XeSS-FG's generated and real presents back to back (2x).
//
// Cases (each in its own process, IGDEXT_XELL_LOG=1 in all of them):
//   off       IGDEXT_FG_METER unset: nothing installed (factory, swap chain and wrapper vtables unchanged), bursts
//   measure   IGDEXT_FG_METER=measure: DXVK's classes hooked, nothing waits, bursts
//   pace      IGDEXT_FG_METER=1, 5 ms CPU per 16.67 ms frame: even flips, the frame rate kept, added latency ~0
//   heavy     IGDEXT_FG_METER=1, 12 ms CPU: the next frame can absorb only 4.7 ms; holds stay within it, frame rate kept
//   nocouple  IGDEXT_FG_METER=1 with a low latency that ignores the hold: shows what the coupling prevents (informative)
// In every case: no crash, the wrapper's vtable untouched; with the meter in, a present on a copy of the hooked vtable
// fails with DXGI_ERROR_INVALID_CALL and calls nothing.
#include "Stdafx.h"
#include "dll/FgMeter.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---- what XellHook.cpp provides to FgMeter.cpp; the meter's 2 s summaries are read back from here -------------------
static volatile int g_fgState = 1;
int XefgFrameGenerationState() { return g_fgState; }
bool XellLogOn()
{
    static int on = -1;
    if (on < 0) { char b[8]; on = GetEnvironmentVariableA("IGDEXT_XELL_LOG", b, sizeof(b)) > 0; }
    return on == 1;
}
struct MeterLog { double spinAvg = -1, spinMax = -1, addedAvg = -1, addedMax = -1, heldAvg = -1; int xellCapped = 0, summaries = 0; };
static MeterLog g_ml;
void XellLogF(const char* fmt, ...)
{
    char b[2048];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    printf("  log: %s\n", b); fflush(stdout);
    if (const char* p = strstr(b, "spin per present avg")) { sscanf(p, "spin per present avg %lf max %lf", &g_ml.spinAvg, &g_ml.spinMax); ++g_ml.summaries; }
    if (const char* p = strstr(b, "(interval) ")) { int k = 0; if (sscanf(p, "(interval) %d", &k) == 1) g_ml.xellCapped += k; }
    if (const char* p = strstr(b, "held in the game's present avg")) sscanf(p, "held in the game's present avg %lf", &g_ml.heldAvg);
    if (const char* p = strstr(b, "added latency per real frame avg")) sscanf(p, "added latency per real frame avg %lf max %lf", &g_ml.addedAvg, &g_ml.addedMax);
}

static uint64_t QpcNs()   // as FgMeter.cpp and XellHook.cpp (XeLL's report clock)
{
    LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t);
    return (uint64_t) (t.QuadPart / f.QuadPart) * 1000000000ull + (uint64_t) (t.QuadPart % f.QuadPart) * 1000000000ull / (uint64_t) f.QuadPart;
}
static double NowMs() { return (double) QpcNs() / 1e6; }
static void WaitUntil(double t) { while (NowMs() < t) { if (t - NowMs() > 2) Sleep(1); } }

// ---- the OptiScaler-style wrapper: vtable in this executable, forwarding to an inner swap chain ---------------------
struct Wrapper { void* const* vtbl; void* inner; };
typedef HRESULT (STDMETHODCALLTYPE* PFN_P)(void*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* PFN_P1)(void*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_GetParent)(void*, REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_ForHwnd)(void*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const void*, void*, void**);
static void** Vt(void* o) { return *(void***) o; }
static HRESULT STDMETHODCALLTYPE WQI(Wrapper* w, REFIID iid, void** out)
{
    if (iid == __uuidof(IDXGISwapChain1) || iid == __uuidof(IDXGISwapChain) || iid == __uuidof(IUnknown)) { *out = w; return S_OK; }
    *out = nullptr; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE WRef(Wrapper*) { return 1; }
static HRESULT STDMETHODCALLTYPE WNotImpl() { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE WGetParent(Wrapper* w, REFIID iid, void** out) { return ((PFN_GetParent) Vt(w->inner)[6])(w->inner, iid, out); }
static HRESULT STDMETHODCALLTYPE WPresent(Wrapper* w, UINT s, UINT f) { return ((PFN_P) Vt(w->inner)[8])(w->inner, s, f); }
static HRESULT STDMETHODCALLTYPE WPresent1(Wrapper* w, UINT s, UINT f, const DXGI_PRESENT_PARAMETERS* p) { return ((PFN_P1) Vt(w->inner)[22])(w->inner, s, f, p); }
static void* const kWrapVtbl[40] = {
    (void*) WQI, (void*) WRef, (void*) WRef, (void*) WNotImpl, (void*) WNotImpl, (void*) WNotImpl, (void*) WGetParent,
    (void*) WNotImpl, (void*) WPresent, (void*) WNotImpl, (void*) WNotImpl, (void*) WNotImpl, (void*) WNotImpl,
    (void*) WNotImpl, (void*) WNotImpl, (void*) WNotImpl, (void*) WNotImpl, (void*) WNotImpl, (void*) WNotImpl,
    (void*) WNotImpl, (void*) WNotImpl, (void*) WNotImpl, (void*) WPresent1,
};

static int g_fail = 0;
static void Check(bool ok, const char* what) { printf("  %s: %s\n", ok ? "ok  " : "FAIL", what); if (!ok) ++g_fail; }

// the flip intervals; a stall of the test machine itself (Wine on a shared 2-CPU box stalls now and then for tens of
// ms) shows as an interval over 2.5x the expected spacing and is counted apart, not in the spread
struct Spacing { double mean, sd; int shortPct, stalls; };
static Spacing Intervals(const double* t, int from, int to, double expected)
{
    std::vector<double> iv; int stalls = 0;
    for (int i = from + 1; i < to; ++i) { const double d = t[i] - t[i - 1]; if (d > 2.5 * expected) ++stalls; else iv.push_back(d); }
    double sum = 0; for (double d : iv) sum += d;
    const double mean = iv.empty() ? 0 : sum / iv.size();
    double var = 0; int sh = 0;
    for (double d : iv) { var += (d - mean) * (d - mean); sh += d < 0.5 * mean; }
    return { mean, iv.empty() ? 0 : std::sqrt(var / iv.size()), iv.empty() ? 0 : (int) (100 * sh / iv.size()), stalls };
}

static int Child(const std::string& mode)
{
    const bool off = mode == "off", measure = mode == "measure", heavy = mode == "heavy", nocouple = mode == "nocouple";
    printf("== %s: IGDEXT_FG_METER=%s, meter %s\n", mode.c_str(), off ? "(unset)" : measure ? "measure" : "1",
           FgMeterOn() ? "pacing" : FgMeterWanted() ? "measuring" : "off");
    wchar_t dir[MAX_PATH]; DWORD n = GetModuleFileNameW(nullptr, dir, MAX_PATH);
    while (n && dir[n - 1] != L'\\' && dir[n - 1] != L'/') --n;
    dir[n] = 0;
    HMODULE dx = LoadLibraryW((std::wstring(dir) + L"fgmeter_fake_dxgi.dll").c_str());
    if (!dx) { printf("FAIL: fgmeter_fake_dxgi.dll not loaded (%lu)\n", GetLastError()); return 1; }
    auto factoryOf = (void* (*)()) GetProcAddress(dx, "FakeDxgiFactory");
    auto flipsOf = (int (*)(void*, const double**)) GetProcAddress(dx, "FakeDxgiFlips");
    void* factory = factoryOf();

    // the game's swap chain (DXVK's), wrapped by "OptiScaler"
    void* gameSc = nullptr;
    ((PFN_ForHwnd) Vt(factory)[15])(factory, nullptr, nullptr, nullptr, nullptr, nullptr, &gameSc);
    Wrapper wrap = { kWrapVtbl, gameSc };
    void* wrapBefore[40]; memcpy(wrapBefore, kWrapVtbl, sizeof(wrapBefore));
    void* facBefore[30]; memcpy(facBefore, Vt(factory), sizeof(facBefore));
    void* scBefore[40]; memcpy(scBefore, Vt(gameSc), sizeof(scBefore));

    // xefgSwapChainD3D12InitFromSwapChain(wrapper): XeSS-FG creates its swap chain through the factory inside the call
    int ctx = 0;
    FgMeterInitBegin(&wrap, nullptr);
    void* xefgSc = nullptr;
    ((PFN_ForHwnd) Vt(factory)[15])(factory, nullptr, nullptr, nullptr, nullptr, nullptr, &xefgSc);
    FgMeterInitEnd(&ctx, 0);
    FgMeterNoteFrames(1, "test");

    Check(!memcmp(wrapBefore, kWrapVtbl, sizeof(wrapBefore)), "the wrapper's vtable (outside DXVK's dxgi) is not patched");
    const bool facHooked = memcmp(facBefore, Vt(factory), sizeof(facBefore)) != 0, scHooked = memcmp(scBefore, Vt(gameSc), sizeof(scBefore)) != 0;
    if (off) Check(!FgMeterWanted() && !facHooked && !scHooked, "meter off with IGDEXT_XELL_LOG=1: nothing installed");
    else Check(facHooked && scHooked, "DXVK's factory and swap chain class hooked");
    if (!off)
    {
        // a copy of the hooked vtable (as another hook might make): no original of another class is called
        void* copy[40]; memcpy(copy, Vt(xefgSc), sizeof(copy));
        struct { void** v; } stranger = { copy };
        const double* t; const int before = flipsOf(xefgSc, &t);
        const HRESULT hr = ((PFN_P) copy[8])(&stranger, 0, 0);
        Check(hr == DXGI_ERROR_INVALID_CALL && flipsOf(xefgSc, &t) == before, "a present on an unknown vtable fails and calls nothing");
    }

    // the game loop: 60 rendered frames per second (GPU-bound), 2x
    const double P = 16.667, C = heavy ? 12.0 : 5.0;
    const int frames = 480, from = 150;   // measured from 2.5 s on (the meter and the low latency settled)
    double inPresent = 0, inPresentMax = 0, latency = 0, sleepSum = 0; int measured = 0;
    double next = NowMs() + 20, prevReady = 0, firstStart = 0, lastStart = 0;
    for (int f = 0; f < frames; ++f)
    {
        const double s0 = NowMs();
        WaitUntil(next);                                       // the low latency's wait
        const double simStart = NowMs();
        WaitUntil(simStart + C);                               // simulation and render submit
        const double ready = std::max(NowMs(), prevReady + P); // the GPU's pace
        WaitUntil(ready); prevReady = ready;
        FgMeterRealFrame(&ctx, (uint32_t) f + 1);              // xefgSwapChainTagFrameConstants
        const uint64_t prS = QpcNs();                          // XELL_PRESENT_START
        ((PFN_P) Vt(&wrap)[8])(&wrap, 0, 0);                   // the game presents through the wrapper ...
        ((PFN_P) Vt(xefgSc)[8])(xefgSc, 0, 0);                 // ... XeSS-FG: the generated frame
        ((PFN_P) Vt(xefgSc)[8])(xefgSc, 0, 0);                 // ... and the real one
        const uint64_t prE = QpcNs();                          // XELL_PRESENT_END
        const double* flips; const int nf = flipsOf(xefgSc, &flips);
        // the low latency: the next frame starts at the present end (without the meter's hold) + frame time - CPU time
        const double held = FgMeterHeldMs(prS, prE);
        const double endMs = (double) prE / 1e6 - (nocouple ? 0 : held);
        const double start = endMs + P - C;
        FgMeterXellFrame(held, start - NowMs(), start - endMs);
        next = start;
        if (f >= from)
        {
            const double d = (double) (prE - prS) / 1e6;
            inPresent += d; inPresentMax = std::max(inPresentMax, d);
            latency += flips[nf - 1] - simStart; sleepSum += simStart - s0;
            if (!measured) firstStart = simStart;
            lastStart = simStart; ++measured;
        }
    }
    const double* flips; const int nf = flipsOf(xefgSc, &flips);
    const Spacing sp = Intervals(flips, 2 * from, nf, P / 2);
    const double period = (lastStart - firstStart) / (measured - 1);
    printf("RESULT %-8s game thread in Present %.2f ms/frame (max %.2f), waiting in the low latency %.2f ms/frame;"
           " flips: interval mean %.2f sd %.2f ms, short %d%% (%d host stalls left out); real frame period %.2f ms; sim start -> real flip %.2f ms;"
           " meter log: held %.2f ms, added latency avg %.3f max %.3f ms, spin per present avg %.3f max %.3f ms, xell-slack caps %d\n",
           mode.c_str(), inPresent / measured, inPresentMax, sleepSum / measured, sp.mean, sp.sd, sp.shortPct, sp.stalls, period,
           latency / measured, g_ml.heldAvg, g_ml.addedAvg, g_ml.addedMax, g_ml.spinAvg, g_ml.spinMax, g_ml.xellCapped);
    if (off || measure) Check(sp.shortPct > 30, "without pacing the flips come in bursts (the problem)");
    if (FgMeterOn() && !nocouple)
    {
        Check(std::fabs(period - P) < 0.02 * P, "the real frame rate is kept (the hold is absorbed by the low latency's wait)");
        Check(g_ml.addedAvg >= 0 && g_ml.addedAvg < 0.1, "added latency per real frame under 0.1 ms on average");
        // the spin is planned at 0.05-0.5 ms; a measured maximum above that is the test machine descheduling the thread
        Check(g_ml.spinAvg >= 0 && g_ml.spinAvg <= 0.1, "spin per present under 0.1 ms on average (planned at most 0.5 ms)");
        if (!heavy) Check(sp.shortPct == 0 && sp.sd < 1.0, "even flips: no bursts, sd under 1 ms");
        else Check(g_ml.xellCapped > 0 && inPresent / measured < P - C, "holds kept within what the next frame absorbs");
    }
    if (nocouple) printf("  (informative: without the coupling every frame starts later by the hold: period %.2f ms instead of %.2f)\n", period, P);
    FreeLibrary(dx);
    return g_fail ? 1 : 0;
}

int main(int argc, char** argv)
{
    if (argc > 1) return Child(argv[1]);
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    struct { const char* mode; const char* meter; } cases[] = {
        { "off", nullptr }, { "measure", "measure" }, { "pace", "1" }, { "heavy", "1" }, { "nocouple", "1" } };
    int failed = 0;
    for (auto& c : cases)
    {
        SetEnvironmentVariableA("IGDEXT_XELL_LOG", "1");   // ClawDeck's default: must not install anything by itself
        SetEnvironmentVariableA("IGDEXT_FG_METER", c.meter);
        std::string cmd = std::string("\"") + exe + "\" " + c.mode;
        STARTUPINFOA si = { sizeof(si) }; PROCESS_INFORMATION pi = {};
        if (!CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) { printf("FAIL: %s not started\n", c.mode); ++failed; continue; }
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        printf("== %s: %s\n", c.mode, code == 0 ? "PASS" : "FAIL");
        failed += code != 0;
    }
    printf(failed ? "%d case(s) failed\n" : "all cases passed\n", failed);
    return failed ? 1 : 0;
}
