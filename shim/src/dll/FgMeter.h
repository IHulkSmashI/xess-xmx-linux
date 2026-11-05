// FgMeter.h - even flips for XeSS frame generation (FgMeter.cpp): what Intel's Windows driver does with
// SetNumGeneratedFrames / SetPresentSequenceNumber, done in the shim on the presents XeSS-FG makes.
//   IGDEXT_FG_METER=1        pace XeSS-FG's presents
//   IGDEXT_FG_METER=measure  measure only: the hooks go in, nothing waits (a pacing summary every 2 s with IGDEXT_XELL_LOG=1)
//   IGDEXT_FG_METER=0/unset  off: nothing is installed, whatever IGDEXT_XELL_LOG says (the default)
#pragma once
#include <cstdint>

// read once: pacing on (IGDEXT_FG_METER=1)
bool FgMeterOn();
// the hooks are needed: IGDEXT_FG_METER=1 or measure
bool FgMeterWanted();
// around xefgSwapChainD3D12InitFromSwapChain(Desc): Begin before the call (appSwapChain: the game's swap chain the
// first form hands over, or null; factory: the IDXGIFactory2 of the second form, or null), End after it with its
// result. Only a swap chain XeSS-FG creates in between, on that thread, through DXVK's own factory, is paced.
void FgMeterInitBegin(void* appSwapChain, void* factory);
void FgMeterInitEnd(void* ctx, int32_t result);
// xefgSwapChainDestroy: the swap chain of that context is no longer paced
void FgMeterContextDestroyed(void* ctx);
// a rendered frame of that context (xefgSwapChainTagFrameConstants' present id)
void FgMeterRealFrame(void* ctx, uint32_t presentId);
// the number of generated frames per rendered frame, wherever the shim learns it (XeSS-FG's count, the driver call);
// used when there are no present ids, and logged
void FgMeterNoteFrames(uint32_t generated, const char* source);

// XeLL coupling (XellHook.cpp's low latency, AutoPaceWait). On Windows the driver meters the flips without holding the
// presenting thread, so XeLL's present-end marker is not moved by the pacing. Here the hold happens inside Present, so
// the present end XeLL reports is later by the hold. The low latency asks how long the meter held inside a frame's
// present (XeLL's present start / end, QPC nanoseconds) and takes it off that present end, so the next frame starts
// when it would have without the meter: the hold replaces part of xellSleep's wait instead of adding to it.
double FgMeterHeldMs(uint64_t fromNs, uint64_t toNs);
// after the low latency computed the next frame's start: heldMs as above, leftMs its wait (negative: late), slackMs the
// time from the frame's present end without the hold to that start (how much hold the next frames can absorb). The
// meter logs the added latency (the part of the hold that made the next frame start late) and keeps later holds
// within the slack.
void FgMeterXellFrame(double heldMs, double leftMs, double slackMs);

// XellHook.cpp: frame generation on (1), off (0) or not seen (-1), from xefgSwapChainSetEnabled / xellSetFgEnabled
int XefgFrameGenerationState();
// XellHook.cpp: C:\igdext_xell.log (IGDEXT_XELL_LOG)
bool XellLogOn();
void XellLogF(const char* fmt, ...);
