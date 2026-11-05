// fgpacer_test.cpp - FgPacer (shim/src/dll/FgPacer.h) on simulated XeSS-FG present streams, no Windows needed:
//   g++ -std=c++17 -O2 -Wall -I shim/src/dll shim/tests/fgpacer_test.cpp -o fgpacer_test && ./fgpacer_test
// XeSS-FG without the driver's pacing: per rendered frame its generated frames and the real one reach Present back
// to back as soon as the GPU has them. The presenting thread is synchronous (the next present cannot arrive before
// the previous one returned). The test checks that the paced presents are evenly spaced, that no present waits more
// than one real-frame interval, and that nothing is paced without generated frames.
#include "FgPacer.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace
{
struct Result { double meanMs, sdMs, localSdMs, minMs, maxMs, maxDelayMs, shortShare; int presents, paced, capped; };

struct Scenario
{
    const char* name;
    double frameMs;          // real frame interval
    double jitterMs;         // its standard deviation
    int perReal;             // presents per real frame (1 = no generated frames)
    bool marks = true;       // XeSS-FG present ids reach the pacer
    int noted = -1;          // generated frames reported elsewhere
    double rampTo = 0;       // frame interval at the end (a scene getting heavier), 0 = constant
    int gapAt = -1;          // a 300 ms hitch before this real frame
};

Result Run(const Scenario& s, bool pace, unsigned seed = 1)
{
    std::mt19937 rng(seed);
    std::normal_distribution<double> jitter(0.0, s.jitterMs);
    FgPacer p;
    p.NoteGenerated(s.noted);
    const int frames = 2000;
    const double presentCostMs = 0.15, burstGapMs = 0.25;
    double t = 1000, lastOut = 0;
    std::vector<double> outs, delays;
    int paced = 0, capped = 0;
    for (int f = 0; f < frames; ++f)
    {
        double ft = s.rampTo > 0 ? s.frameMs + (s.rampTo - s.frameMs) * f / frames : s.frameMs;
        t += std::max(1.0, ft + jitter(rng));
        if (f == s.gapAt) t += 300;
        if (s.marks) p.RealFrame(t - 6.0, (uint32_t) f + 1);   // the game tags the frame a little before it presents
        for (int k = 0; k < s.perReal; ++k)
        {
            double arrival = std::max(t + k * burstGapMs, lastOut + presentCostMs);
            double out = arrival;
            if (pace)
            {
                FgPacer::Plan pl = p.Schedule(arrival);
                if (pl.paced) { out = pl.targetMs; ++paced; }
                if (pl.capped) ++capped;
            }
            p.Presented(out);
            lastOut = out + presentCostMs;
            delays.push_back(out - (t + k * burstGapMs));
            outs.push_back(out);
        }
    }
    // skip the start and the hitch: from the 200th present on, intervals under 50 ms
    std::vector<double> iv;
    for (size_t i = 201; i < outs.size(); ++i) if (outs[i] - outs[i - 1] < 50) iv.push_back(outs[i] - outs[i - 1]);
    double sum = 0; for (double d : iv) sum += d;
    const double mean = sum / iv.size();
    double var = 0, mn = 1e9, mx = 0; int shortN = 0;
    for (double d : iv) { var += (d - mean) * (d - mean); mn = std::min(mn, d); mx = std::max(mx, d); shortN += d < mean * 0.5; }
    // frame to frame: the spread of one interval against the next (a slow change of the frame rate does not count)
    double local = 0;
    for (size_t i = 1; i < iv.size(); ++i) local += (iv[i] - iv[i - 1]) * (iv[i] - iv[i - 1]);
    return { mean, std::sqrt(var / iv.size()), std::sqrt(local / (2 * (iv.size() - 1))), mn, mx, *std::max_element(delays.begin() + 200, delays.end()),
             (double) shortN / iv.size(), (int) outs.size(), paced, capped };
}

int g_fail = 0;
void Check(bool ok, const char* what, const Scenario& s)
{
    if (!ok) { ++g_fail; std::printf("  FAIL %s: %s\n", s.name, what); }
}

void Show(const char* tag, const Result& r)
{
    std::printf("  %-5s interval mean %6.2f sd %5.2f (frame to frame %5.2f) min %5.2f max %6.2f ms, short %4.1f%%, max delay %5.2f ms, paced %d/%d capped %d\n",
                tag, r.meanMs, r.sdMs, r.localSdMs, r.minMs, r.maxMs, 100 * r.shortShare, r.maxDelayMs, r.paced, r.presents, r.capped);
}
}

int main()
{
    const Scenario paced[] = {
        { "2x, 60 fps real -> 120 Hz", 16.67, 0.4, 2 },
        { "2x, 50 fps real, jittery", 20.0, 1.5, 2 },
        { "3x, 40 fps real -> 120 Hz", 25.0, 0.5, 3 },
        { "4x, 30 fps real", 33.3, 0.8, 4 },
        { "2x, no present ids (count noted)", 16.67, 0.4, 2, false, 1 },
        { "3x, no present ids (count noted)", 25.0, 0.5, 3, false, 2 },
        { "2x, scene gets heavier 14 -> 22 ms", 14.0, 0.4, 2, true, -1, 22.0 },
        { "2x, a 300 ms hitch", 16.67, 0.4, 2, true, -1, 0, 1000 },
    };
    for (const Scenario& s : paced)
    {
        std::printf("%s\n", s.name);
        Result off = Run(s, false), on = Run(s, true);
        Show("off", off); Show("on", on);
        const double ideal = s.rampTo > 0 ? 0 : s.frameMs / s.perReal;
        // the paced stream is even: no burst intervals left, spread within 20% of the ideal spacing
        Check(on.shortShare < 0.01, "burst intervals left", s);
        Check(on.localSdMs < std::max(0.12 * on.meanMs, 2 * s.jitterMs / s.perReal + 0.3), "intervals not even", s);
        if (ideal > 0) Check(std::fabs(on.meanMs - ideal) < 0.05 * ideal, "frame rate changed", s);
        // the unpaced stream had bursts (the problem), and pacing never costs more than one real interval
        Check(off.shortShare > 0.3, "the simulation has no bursts", s);
        Check(on.maxDelayMs <= (s.rampTo > 0 ? s.rampTo : s.frameMs) + 4 * s.jitterMs + 0.5, "a present waited more than one interval", s);
    }
    {
        Scenario s = { "1 present per real frame (FG on, nothing generated)", 16.67, 0.4, 1 };
        std::printf("%s\n", s.name);
        Result on = Run(s, true);
        Show("on", on);
        Check(on.paced == 0, "paced without generated frames", s);
    }
    std::printf(g_fail ? "%d check(s) failed\n" : "all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
