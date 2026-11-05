// fgmeter_wine_test.cpp - FgMeter.cpp end to end, on fake DXGI objects, in real time (Windows or Wine):
//   x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -DNOMINMAX -DWIN32_LEAN_AND_MEAN -DINTC_IGDEXT_D3D11 -DINTC_IGDEXT_D3D12
//     -I<DirectX-Headers>/include/directx -Ishim/src -Ishim/src/legacy -Ishim/d3dext-sdk/include/public
//     shim/tests/fgmeter_wine_test.cpp shim/src/dll/FgMeter.cpp -o fgmeter_wine_test.exe
//   IGDEXT_FG_METER=1 wine fgmeter_wine_test.exe;  IGDEXT_FG_METER=0 wine fgmeter_wine_test.exe
// (any d3d12.h with the raytracing types: mingw-w64's own is too old for igdext.h)
// A fake factory and swap chain stand in for DXVK's (vtables in read-only data, Present calling Present1 through the
// vtable as DXVK does). The test does what XeSS-FG does: creates its swap chain through the factory inside the init
// call, then per rendered frame tags the frame and presents a generated and the real frame back to back. It prints the
// meter's log lines and the intervals the fake Present1 saw; exit code 1 when the meter is on and they are not even.
#include "Stdafx.h"
#include "dll/FgMeter.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

// what XellHook.cpp provides to FgMeter.cpp
static volatile int g_fgState = 1;
int XefgFrameGenerationState() { return g_fgState; }
bool XellLogOn() { return true; }
void XellLogF(const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n'); fflush(stdout);
}

static double NowMs()
{
    LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t);
    return t.QuadPart * 1000.0 / f.QuadPart;
}

struct FakeObj { void* const* vtbl; };
static std::vector<double> g_flips;
static int g_presentCalls = 0, g_present1Calls = 0;

static HRESULT STDMETHODCALLTYPE ScQI(FakeObj* self, REFIID iid, void** out)
{
    if (iid == __uuidof(IDXGISwapChain1) || iid == __uuidof(IDXGISwapChain) || iid == __uuidof(IUnknown)) { *out = self; return S_OK; }
    *out = nullptr; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE Ref(FakeObj*) { return 1; }
static HRESULT STDMETHODCALLTYPE ScPresent1(FakeObj*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*)
{
    ++g_present1Calls; g_flips.push_back(NowMs()); return S_OK;
}
static HRESULT STDMETHODCALLTYPE ScPresent(FakeObj* self, UINT sync, UINT flags)
{
    // as DXVK: Present is Present1, through the vtable
    ++g_presentCalls;
    typedef HRESULT (STDMETHODCALLTYPE* P1)(FakeObj*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    return ((P1) self->vtbl[22])(self, sync, flags, nullptr);
}
static void* const kScVtbl[40] = {
    (void*) ScQI, (void*) Ref, (void*) Ref, nullptr, nullptr, nullptr, nullptr, nullptr,
    (void*) ScPresent, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, (void*) ScPresent1,
};
static FakeObj g_sc = { kScVtbl };

static HRESULT STDMETHODCALLTYPE FacQI(FakeObj* self, REFIID iid, void** out)
{
    if (iid == __uuidof(IDXGIFactory2) || iid == __uuidof(IDXGIFactory) || iid == __uuidof(IUnknown)) { *out = self; return S_OK; }
    *out = nullptr; return E_NOINTERFACE;
}
static HRESULT STDMETHODCALLTYPE FacCreateForHwnd(FakeObj*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                  const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, void** out)
{
    *out = &g_sc; return S_OK;
}
static HRESULT STDMETHODCALLTYPE NotImpl() { return E_NOTIMPL; }
static void* const kFacVtbl[30] = {
    (void*) FacQI, (void*) Ref, (void*) Ref, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    (void*) NotImpl, nullptr, nullptr, nullptr, nullptr, (void*) FacCreateForHwnd, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, (void*) NotImpl,
};
static FakeObj g_factory = { kFacVtbl };

static void SpinUntil(double t) { while (NowMs() < t) { if (t - NowMs() > 2) Sleep(1); } }

int main()
{
    const bool on = FgMeterOn();
    printf("IGDEXT_FG_METER: %s\n", on ? "on" : "off");
    int ctx = 0;
    // XeSS-FG's xefgSwapChainD3D12InitFromSwapChainDesc: it creates its swap chain through the factory
    FgMeterInitBegin(nullptr, &g_factory);
    typedef HRESULT (STDMETHODCALLTYPE* Create)(void*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const void*, void*, void**);
    void* sc = nullptr;
    ((Create) (*(void* const**) &g_factory)[15])(&g_factory, nullptr, nullptr, nullptr, nullptr, nullptr, &sc);
    FgMeterInitEnd(&ctx, 0);
    FgMeterNoteFrames(1, "test");
    typedef HRESULT (STDMETHODCALLTYPE* Present)(void*, UINT, UINT);
    Present present = (Present) (*(void* const**) sc)[8];

    // 60 rendered frames per second, 2x: a generated and the real frame back to back
    const double frameMs = 16.667;
    const int frames = 360;
    double t = NowMs() + 50;
    for (int f = 0; f < frames; ++f)
    {
        if (f == 300) g_fgState = 0;   // frame generation off for the last frames: straight through
        SpinUntil(t);
        FgMeterRealFrame(&ctx, (uint32_t) f + 1);
        present(sc, 0, 0);
        if (g_fgState) present(sc, 0, 0);
        t += frameMs;
    }
    // intervals while frame generation was on, after the start
    std::vector<double> iv;
    for (size_t i = 61; i < 600 && i < g_flips.size(); ++i) iv.push_back(g_flips[i] - g_flips[i - 1]);
    double sum = 0, mx = 0, mn = 1e9; int shortN = 0;
    for (double d : iv) { sum += d; mx = d > mx ? d : mx; mn = d < mn ? d : mn; }
    const double mean = sum / iv.size();
    double var = 0;
    for (double d : iv) { var += (d - mean) * (d - mean); shortN += d < mean * 0.5; }
    const double sd = std::sqrt(var / iv.size());
    printf("flips: %zu (Present %d, Present1 %d), interval mean %.2f sd %.2f min %.2f max %.2f ms, short %d of %zu\n",
           g_flips.size(), g_presentCalls, g_present1Calls, mean, sd, mn, mx, shortN, iv.size());
    const bool once = g_present1Calls == g_presentCalls;   // metered once per present, Present1 not twice
    const bool even = shortN <= (int) iv.size() / 50 && sd < 1.5;
    if (on) printf("%s\n", even && once ? "PASS: even" : "FAIL");
    else printf("%s\n", shortN > (int) iv.size() / 3 && once ? "PASS: bursts without the meter (as expected)" : "FAIL: no bursts without the meter?");
    return on ? !(even && once) : !(shortN > (int) iv.size() / 3 && once);
}
