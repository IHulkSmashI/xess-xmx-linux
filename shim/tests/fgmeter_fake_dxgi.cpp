// fgmeter_fake_dxgi.cpp - the stand-in for DXVK's dxgi.dll in fgmeter_wine_test.cpp (built as fgmeter_fake_dxgi.dll
// next to the test; FgMeter.cpp built with -DFGMETER_TEST_DXGI=L"fgmeter_fake_dxgi.dll" takes it for DXVK's module).
// A factory and a swap chain class whose vtables and functions lie in this module, as DXVK's do; Present calls Present1
// through the vtable as DXVK's does; every Present1 is a flip, stamped on QPC.
#include <windows.h>
#include <dxgi1_2.h>
#include <vector>

namespace
{
struct FakeSc { void* const* vtbl; void* factory; std::vector<double> flips; };
struct FakeFactory { void* const* vtbl; };

double NowMs()
{
    LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t);
    return t.QuadPart * 1000.0 / f.QuadPart;
}

HRESULT STDMETHODCALLTYPE ScQI(FakeSc* self, REFIID iid, void** out)
{
    if (iid == __uuidof(IDXGISwapChain1) || iid == __uuidof(IDXGISwapChain) || iid == __uuidof(IUnknown)) { *out = self; return S_OK; }
    *out = nullptr; return E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE Ref(void*) { return 1; }
HRESULT STDMETHODCALLTYPE NotImpl() { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE ScGetParent(FakeSc* self, REFIID, void** out) { *out = self->factory; return S_OK; }
HRESULT STDMETHODCALLTYPE ScPresent1(FakeSc* self, UINT, UINT, const DXGI_PRESENT_PARAMETERS*)
{
    self->flips.push_back(NowMs());
    return S_OK;
}
HRESULT STDMETHODCALLTYPE ScPresent(FakeSc* self, UINT sync, UINT flags)
{
    typedef HRESULT (STDMETHODCALLTYPE* P1)(FakeSc*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    return ((P1) self->vtbl[22])(self, sync, flags, nullptr);   // as DXVK: Present is Present1, through the vtable
}
void* const kScVtbl[40] = {
    (void*) ScQI, (void*) Ref, (void*) Ref, (void*) NotImpl, (void*) NotImpl, (void*) NotImpl, (void*) ScGetParent,
    (void*) NotImpl, (void*) ScPresent, (void*) NotImpl, (void*) NotImpl, (void*) NotImpl, (void*) NotImpl,
    (void*) NotImpl, (void*) NotImpl, (void*) NotImpl, (void*) NotImpl, (void*) NotImpl, (void*) NotImpl,
    (void*) NotImpl, (void*) NotImpl, (void*) NotImpl, (void*) ScPresent1,
};

FakeFactory g_factory;
FakeSc* NewSc() { FakeSc* s = new FakeSc; s->vtbl = kScVtbl; s->factory = &g_factory; return s; }

HRESULT STDMETHODCALLTYPE FacQI(FakeFactory* self, REFIID iid, void** out)
{
    if (iid == __uuidof(IDXGIFactory2) || iid == __uuidof(IDXGIFactory1) || iid == __uuidof(IDXGIFactory) || iid == __uuidof(IUnknown))
    { *out = self; return S_OK; }
    *out = nullptr; return E_NOINTERFACE;
}
HRESULT STDMETHODCALLTYPE FacCreateSwapChain(FakeFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, void** out) { *out = NewSc(); return S_OK; }
HRESULT STDMETHODCALLTYPE FacCreateForHwnd(FakeFactory*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                           const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, void** out)
{ *out = NewSc(); return S_OK; }
HRESULT STDMETHODCALLTYPE FacCreateForComposition(FakeFactory*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, void** out)
{ *out = NewSc(); return S_OK; }
void* const kFacVtbl[30] = {
    (void*) FacQI, (void*) Ref, (void*) Ref, (void*) NotImpl, (void*) NotImpl, (void*) NotImpl, (void*) NotImpl,
    (void*) NotImpl, (void*) NotImpl, (void*) NotImpl, (void*) FacCreateSwapChain, (void*) NotImpl, (void*) NotImpl,
    (void*) NotImpl, (void*) NotImpl, (void*) FacCreateForHwnd, (void*) NotImpl, (void*) NotImpl, (void*) NotImpl,
    (void*) NotImpl, (void*) NotImpl, (void*) NotImpl, (void*) NotImpl, (void*) NotImpl, (void*) FacCreateForComposition,
};
}

extern "C" __declspec(dllexport) void* FakeDxgiFactory() { g_factory.vtbl = kFacVtbl; return &g_factory; }
extern "C" __declspec(dllexport) int FakeDxgiFlips(void* sc, const double** times)
{
    FakeSc* s = (FakeSc*) sc;
    *times = s->flips.data();
    return (int) s->flips.size();
}
