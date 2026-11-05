// FgMeter.h - even flips for XeSS frame generation (FgMeter.cpp): what Intel's Windows driver does with
// SetNumGeneratedFrames / SetPresentSequenceNumber, done in the shim on the presents XeSS-FG makes.
//   IGDEXT_FG_METER=1   pace XeSS-FG's presents;  0 or unset: no pacing (the default, see FgMeter.cpp)
//   IGDEXT_XELL_LOG=1   also: a pacing summary every 2 s in C:\igdext_xell.log, with the meter on or off
#pragma once
#include <cstdint>

// read once: pacing on (IGDEXT_FG_METER=1)
bool FgMeterOn();
// the hooks are needed: pacing on, or the log on (the summary without pacing, for an A/B)
bool FgMeterWanted();
// around xefgSwapChainD3D12InitFromSwapChain(Desc): Begin before the call (appSwapChain: the game's swap chain the
// first form hands over, or null; factory: the IDXGIFactory2 of the second form, or null), End after it with its
// result. The swap chain XeSS-FG creates in between (or else the game's) is the one whose presents are paced.
void FgMeterInitBegin(void* appSwapChain, void* factory);
void FgMeterInitEnd(void* ctx, int32_t result);
// xefgSwapChainDestroy: the swap chain of that context is no longer paced
void FgMeterContextDestroyed(void* ctx);
// a rendered frame of that context (xefgSwapChainTagFrameConstants' present id)
void FgMeterRealFrame(void* ctx, uint32_t presentId);
// the number of generated frames per rendered frame, wherever the shim learns it (XeSS-FG's count, the driver call);
// used when there are no present ids, and logged
void FgMeterNoteFrames(uint32_t generated, const char* source);

// XellHook.cpp: frame generation on (1), off (0) or not seen (-1), from xefgSwapChainSetEnabled / xellSetFgEnabled
int XefgFrameGenerationState();
// XellHook.cpp: C:\igdext_xell.log (IGDEXT_XELL_LOG)
bool XellLogOn();
void XellLogF(const char* fmt, ...);
