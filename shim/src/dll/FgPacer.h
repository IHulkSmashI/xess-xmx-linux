// FgPacer.h - the schedule of FgMeter.cpp (even flips for XeSS frame generation), without any Windows code so that
// shim/tests/fgpacer_test.cpp can run it on recorded or simulated present times. All times in milliseconds of one
// monotonic clock.
//
// What it does: XeSS-FG presents the generated frames and the real one of each rendered frame back to back as soon as
// the GPU has them (on Windows the Intel driver spaces them; SetNumGeneratedFrames / SetPresentSequenceNumber). Here
// each present is held until the previous one + interval / presents-per-real-frame, as AMD's FSR 3 frame interpolation
// swap chain does on its presenter thread (FrameInterpolationSwapchainDX12.cpp: half the moving average of 10 frame
// times, less a share of their variance and a 0.1 ms safety margin, then a hybrid sleep + spin to the target).
//  - interval: the moving average of the last 16 real frames, from the frame marks (XeSS-FG's per-frame present id,
//    one per rendered frame). Without marks: the present rate times the presents per real frame noted elsewhere.
//  - presents per real frame (R): counted between the marks, so a frame XeSS-FG did not interpolate counts as it is;
//    below 1.5 nothing is paced (frame generation produces no frames, e.g. in a menu).
//  - the spacing is slightly under interval / R (half a standard deviation of the interval, 0.1 ms margin): a spacing
//    above the true one would add a little delay every frame until the cap; under it, the first present of each burst
//    goes out when it arrives and only the ones behind it wait.
//  - never more than one interval (at most 100 ms) after the present arrived; no wait after a gap (a hitch, a load).
#pragma once
#include <cmath>
#include <cstdint>

class FgPacer
{
public:
    static constexpr int kMarks = 17;          // 16 intervals
    static constexpr int kArrivals = 33;       // 32 intervals, the fallback without marks
    static constexpr double kGapMs = 100.0;    // longer than this between marks or presents: start over
    static constexpr double kMarginMs = 0.1;

    struct Plan { double targetMs; double spacingMs; double intervalMs; double perReal; bool capped; bool paced; };

    void Reset() { m_nMarks = 0; m_nArr = 0; m_prevOut = 0; m_sinceMark = 0; m_haveId = false; }

    // the generated frames per real frame another source reported (XeSS-FG's count, the driver call), -1 unknown
    void NoteGenerated(int n) { m_noted = n; }

    // a rendered frame, with XeSS-FG's present id: the same id twice is one frame
    void RealFrame(double nowMs, uint32_t id)
    {
        if (m_haveId && id == m_lastId) return;
        m_haveId = true; m_lastId = id;
        if (m_nMarks && nowMs - m_mark[(m_nMarks - 1) % kMarks] > kGapMs) m_nMarks = 0;
        if (m_nMarks) m_count[(m_nMarks - 1) % kMarks] = m_sinceMark;   // presents in the interval that ends now
        m_mark[m_nMarks % kMarks] = nowMs;
        ++m_nMarks;
        m_sinceMark = 0;
    }

    // a present arrives: when it should go out
    Plan Schedule(double arrivalMs)
    {
        Plan p = { arrivalMs, 0, 0, 0, false, false };
        if (m_nArr && arrivalMs - m_arr[(m_nArr - 1) % kArrivals] > kGapMs) m_nArr = 0;
        m_arr[m_nArr % kArrivals] = arrivalMs; ++m_nArr;
        ++m_sinceMark;
        double interval = 0, sd = 0, perReal = 0;
        if (!Estimate(arrivalMs, interval, sd, perReal)) return p;
        p.intervalMs = interval; p.perReal = perReal;
        if (perReal < 1.5) return p;
        const double spacing = (interval - 0.5 * sd) / perReal - kMarginMs;
        p.spacingMs = spacing;
        if (spacing <= 0 || !m_prevOut || arrivalMs - m_prevOut > 2 * interval || arrivalMs - m_prevOut > kGapMs) return p;
        double t = m_prevOut + spacing;
        const double cap = arrivalMs + (interval < kGapMs ? interval : kGapMs);
        if (t > cap) { t = cap; p.capped = true; }
        if (t > arrivalMs) { p.targetMs = t; p.paced = true; }
        return p;
    }

    // the present went out (the real Present was called) at this time
    void Presented(double outMs) { m_prevOut = outMs; }

private:
    bool Estimate(double nowMs, double& interval, double& sd, double& perReal) const
    {
        const int k = m_nMarks < kMarks ? m_nMarks : kMarks;          // marks held
        const double last = k ? m_mark[(m_nMarks - 1) % kMarks] : 0;
        if (k >= 5)
        {
            const double first = m_mark[(m_nMarks - k) % kMarks];
            const double avg = (last - first) / (k - 1);
            // the marks are fresh: within a few intervals (else XeSS-FG stopped tagging frames, use the presents)
            if (avg > 0 && nowMs - last < 4 * avg + 5)
            {
                double var = 0; int presents = 0;
                for (int i = m_nMarks - k; i < m_nMarks - 1; ++i)
                {
                    const double d = m_mark[(i + 1) % kMarks] - m_mark[i % kMarks] - avg;
                    var += d * d;
                    presents += m_count[i % kMarks];
                }
                interval = avg; sd = std::sqrt(var / (k - 1)); perReal = (double) presents / (k - 1);
                return true;
            }
        }
        // no marks: the presents' own rate, and the count noted elsewhere (1 generated frame if none)
        const int a = m_nArr < kArrivals ? m_nArr : kArrivals;
        if (a < 9) return false;
        const double span = m_arr[(m_nArr - 1) % kArrivals] - m_arr[(m_nArr - a) % kArrivals];
        if (span <= 0) return false;
        perReal = m_noted >= 0 ? m_noted + 1.0 : 2.0;
        interval = span / (a - 1) * perReal;
        sd = 0;
        return true;
    }

    double m_mark[kMarks] = {}; int m_count[kMarks] = {}; int m_nMarks = 0; int m_sinceMark = 0;
    double m_arr[kArrivals] = {}; int m_nArr = 0;
    double m_prevOut = 0;
    uint32_t m_lastId = 0; bool m_haveId = false;
    int m_noted = -1;
};
