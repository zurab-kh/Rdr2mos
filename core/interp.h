// Smooth playback of remote movement.
//
//  * Samples are rendered slightly in the past, between two received samples. The delay adapts
//    to the measured send interval and network jitter (a jitter buffer), so a stable connection
//    gets a short delay and a bad one still stays smooth.
//  * The playback clock never jumps: it runs a few percent faster or slower to converge on the
//    target delay, which keeps motion continuous when the network hiccups.
//  * Between samples positions follow a cubic Hermite curve built from the sent velocities,
//    so turns and speed changes are curved instead of zig-zagging from point to point.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include "mathx.h"

namespace coop {

struct MotionSample {
    uint32_t timeMs = 0;  // sender clock
    Vec3 pos;
    Vec3 vel;
    float heading = 0;
    float pitch = 0, roll = 0;
    bool hasVel = false;
};

class MotionBuffer {
public:
    MotionBuffer(uint32_t minDelayMs = 50, uint32_t maxDelayMs = 350) : minDelay_(minDelayMs), maxDelay_(maxDelayMs) {}

    void push(const MotionSample& s, uint32_t localNowMs) {
        if (!samples_.empty()) {
            int32_t step = static_cast<int32_t>(s.timeMs - samples_.back().timeMs);
            if (step <= 0) return;  // duplicate or out of order
            if (step > 3000) clear();  // long gap (teleport, stream re-entry): start over
        }
        // Network timing: (local - sender) is the clock offset plus the one-way delay of this
        // packet. Its running minimum is the fastest delivery; everything above it is jitter.
        int32_t raw = static_cast<int32_t>(localNowMs - s.timeMs);
        if (samples_.empty()) {
            base_ = raw;
            refTime_ = s.timeMs;
        }
        int32_t transit = raw - base_;
        if (transit < -5000 || transit > 5000) {  // sender restarted its clock
            clear();
            base_ = raw;
            refTime_ = s.timeMs;
            transit = 0;
        }
        transits_[transitHead_++ % kWindow] = transit;
        transitCount_ = std::min<uint32_t>(transitCount_ + 1, kWindow);
        if (!samples_.empty()) {
            float step = static_cast<float>(s.timeMs - samples_.back().timeMs);
            interval_ = interval_ <= 0 ? step : interval_ + (step - interval_) * 0.1f;
        }
        samples_.push_back(s);
        while (samples_.size() > 48) samples_.pop_front();
    }

    bool empty() const { return samples_.empty(); }
    const MotionSample& latest() const { return samples_.back(); }
    void clear() {
        samples_.clear();
        transitCount_ = 0;
        transitHead_ = 0;
        interval_ = 0;
        playing_ = false;
    }
    // Current playback delay behind the newest data, for diagnostics.
    uint32_t delayMs() const { return static_cast<uint32_t>(targetDelay_); }

    // Where the entity should be shown at local time `localNowMs`. Call once per frame.
    MotionSample sample(uint32_t localNowMs) {
        if (samples_.empty()) return {};
        updateTargetDelay();
        // Sender time we would like to show now.
        double ideal = static_cast<double>(static_cast<int32_t>(localNowMs - static_cast<uint32_t>(base_) - lowTransit_ -
                                                                refTime_)) -
                       targetDelay_;
        if (!playing_ || std::fabs(ideal - playTime_) > 600.0) {
            playTime_ = ideal;
            playing_ = true;
        } else {
            double dt = static_cast<double>(static_cast<int32_t>(localNowMs - lastLocal_));
            if (dt < 0) dt = 0;
            if (dt > 250) dt = 250;
            double error = ideal - playTime_;
            double rate = 1.0 + std::clamp(error * 0.003, -0.12, 0.12);  // gentle time warping
            playTime_ += dt * rate;
        }
        lastLocal_ = localNowMs;
        uint32_t t = refTime_ + static_cast<uint32_t>(static_cast<int32_t>(std::floor(playTime_)));
        float frac = static_cast<float>(playTime_ - std::floor(playTime_));
        return evaluate(t, frac);
    }

private:
    static constexpr uint32_t kWindow = 40;

    void updateTargetDelay() {
        if (transitCount_ == 0) return;
        int32_t low = transits_[0], high = transits_[0];
        for (uint32_t i = 1; i < transitCount_; ++i) {
            low = std::min(low, transits_[i]);
            high = std::max(high, transits_[i]);
        }
        lowTransit_ = low;
        float jitter = static_cast<float>(high - low);
        float interval = interval_ > 0 ? interval_ : 50.0f;
        float target = interval * 1.1f + jitter + 8.0f;
        target = std::clamp(target, static_cast<float>(minDelay_), static_cast<float>(maxDelay_));
        // Grow quickly when the network gets worse, shrink slowly when it recovers.
        targetDelay_ = target > targetDelay_ ? target : targetDelay_ + (target - targetDelay_) * 0.02f;
    }

    MotionSample evaluate(uint32_t t, float frac) const {
        const MotionSample& first = samples_.front();
        if (static_cast<int32_t>(t - first.timeMs) < 0) return first;
        for (size_t i = 1; i < samples_.size(); ++i) {
            const MotionSample& a = samples_[i - 1];
            const MotionSample& b = samples_[i];
            if (static_cast<int32_t>(t - b.timeMs) >= 0) continue;
            float span = static_cast<float>(b.timeMs - a.timeMs);
            float k = std::clamp((static_cast<float>(t - a.timeMs) + frac) / span, 0.0f, 1.0f);
            return interpolate(i, k, span);
        }
        // Past the newest sample: continue along its velocity for a short while, then hold.
        const MotionSample& last = samples_.back();
        MotionSample out = last;
        float ahead = std::min(static_cast<float>(t - last.timeMs) + frac, 250.0f) / 1000.0f;
        Vec3 v = tangent(samples_.size() - 1);
        out.pos = last.pos + v * ahead;
        out.vel = ahead >= 0.25f ? Vec3{} : v;
        out.timeMs = t;
        return out;
    }

    // Velocity at sample i: the sent one, or a central difference of the neighbours.
    Vec3 tangent(size_t i) const {
        const MotionSample& s = samples_[i];
        if (s.hasVel) return s.vel;
        size_t lo = i > 0 ? i - 1 : i, hi = i + 1 < samples_.size() ? i + 1 : i;
        if (lo == hi) return {};
        float dt = static_cast<float>(samples_[hi].timeMs - samples_[lo].timeMs) / 1000.0f;
        return dt > 0 ? (samples_[hi].pos - samples_[lo].pos) * (1.0f / dt) : Vec3{};
    }

    MotionSample interpolate(size_t i, float k, float spanMs) const {
        const MotionSample& a = samples_[i - 1];
        const MotionSample& b = samples_[i];
        float span = spanMs / 1000.0f;
        Vec3 va = tangent(i - 1), vb = tangent(i);
        MotionSample out;
        out.timeMs = a.timeMs + static_cast<uint32_t>(k * spanMs);
        // Sanity: if the velocities cannot explain the jump (teleport, respawn), go linear.
        float chord = distance(a.pos, b.pos);
        float reach = (length(va) + length(vb)) * span + 0.5f;
        if (chord > reach * 2.0f) {
            out.pos = lerp(a.pos, b.pos, k);
            out.vel = (b.pos - a.pos) * (span > 0 ? 1.0f / span : 0.0f);
        } else {
            float k2 = k * k, k3 = k2 * k;
            float h00 = 2 * k3 - 3 * k2 + 1, h10 = k3 - 2 * k2 + k, h01 = -2 * k3 + 3 * k2, h11 = k3 - k2;
            out.pos = a.pos * h00 + va * (h10 * span) + b.pos * h01 + vb * (h11 * span);
            float d00 = 6 * k2 - 6 * k, d10 = 3 * k2 - 4 * k + 1, d01 = -6 * k2 + 6 * k, d11 = 3 * k2 - 2 * k;
            out.vel = span > 0 ? (a.pos * d00 + b.pos * d01) * (1.0f / span) + va * d10 + vb * d11 : Vec3{};
        }
        out.hasVel = true;
        out.heading = lerpHeading(a.heading, b.heading, k);
        out.pitch = a.pitch + (b.pitch - a.pitch) * k;
        out.roll = a.roll + (b.roll - a.roll) * k;
        return out;
    }

    std::deque<MotionSample> samples_;
    int32_t transits_[kWindow] = {};
    uint32_t transitHead_ = 0, transitCount_ = 0;
    int32_t base_ = 0;
    int32_t lowTransit_ = 0;
    float interval_ = 0;
    float targetDelay_ = 0;
    uint32_t minDelay_, maxDelay_;
    uint32_t refTime_ = 0;  // sender time playback is measured from (first sample after a reset)
    double playTime_ = 0;   // ms since refTime_ shown at the last sample() call
    uint32_t lastLocal_ = 0;
    bool playing_ = false;
};

}  // namespace coop
