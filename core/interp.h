// Smooth playback of remote movement.
//
//  * Samples are rendered slightly in the past, between two received samples. The delay adapts
//    to the measured send interval and network jitter (a jitter buffer), so a stable connection
//    gets a short delay and a bad one still stays smooth.
//  * The playback clock never jumps: it runs a few percent faster or slower to converge on the
//    target delay, which keeps motion continuous when the network hiccups.
//  * Between samples positions follow a cubic Hermite curve built from the sent velocities,
//    so turns and speed changes are curved instead of zig-zagging from point to point.
//  * When data runs out (a loss burst), motion continues along the current curve and then slows
//    to a stop; once data is back, playback glides onto the real path instead of jumping.
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
            if (step < -kGapMs) {
                // Far older than what we have: a stale packet, or the sender's clock went back (its
                // game restarted, or the stream now comes from another machine). A steady run of
                // such samples is a new clock; anything else is ignored.
                bool run = stale_ > 0 && static_cast<int32_t>(s.timeMs - staleLast_) > 0 &&
                           static_cast<int32_t>(s.timeMs - staleLast_) < kGapMs;
                stale_ = run ? stale_ + 1 : 1;
                staleLast_ = s.timeMs;
                if (stale_ < 3) return;
                clear();
            } else if (step <= 0) {
                return;  // duplicate or out of order
            } else if (step > kGapMs) {
                clear();  // long gap (teleport, stream re-entry): start over
            }
        }
        stale_ = 0;
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
        // The window only changes here, so its extremes are found here, not on every frame.
        windowLow_ = windowHigh_ = transits_[0];
        for (uint32_t i = 1; i < transitCount_; ++i) {
            windowLow_ = std::min(windowLow_, transits_[i]);
            windowHigh_ = std::max(windowHigh_, transits_[i]);
        }
        if (!samples_.empty()) {
            // Senders slow down while nothing moves; those pauses say nothing about the update
            // rate during movement, so they must not inflate the delay.
            float step = static_cast<float>(s.timeMs - samples_.back().timeMs);
            if (interval_ <= 0) interval_ = std::min(step, 150.0f);
            else if (step < interval_ * 2.5f) interval_ += (step - interval_) * 0.1f;
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
        haveOut_ = false;
        offset_ = {};
        stale_ = 0;
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
        float dt = haveOut_ ? static_cast<float>(static_cast<int32_t>(localNowMs - lastLocal_)) / 1000.0f : 0.0f;
        lastLocal_ = localNowMs;
        uint32_t t = refTime_ + static_cast<uint32_t>(static_cast<int32_t>(std::floor(playTime_)));
        float frac = static_cast<float>(playTime_ - std::floor(playTime_));
        MotionSample out = evaluate(t, frac);

        // Recovery from a guess: while playback ran past the newest sample it continued along the
        // predicted curve. When real data arrives, the difference is faded out over a moment.
        const uint32_t newest = samples_.back().timeMs;
        if (haveOut_) {
            dt = std::clamp(dt, 0.0f, 0.25f);
            if (wasGuessing_ && newest != lastNewest_) {
                Vec3 jump = lastOut_.pos + lastOut_.vel * dt - out.pos;
                offset_ = length(jump) < kMaxGlide ? jump : Vec3{};
            } else {
                offset_ = offset_ * std::exp(-dt / kGlideSec);
            }
        }
        if (length(offset_) < 0.002f) offset_ = {};
        out.pos = out.pos + offset_;
        out.vel = out.vel - offset_ * (1.0f / kGlideSec);  // the glide is part of the motion
        wasGuessing_ = static_cast<int32_t>(t - newest) >= 0;
        lastNewest_ = newest;
        lastOut_ = out;
        haveOut_ = true;
        return out;
    }

    // Where the entity will be `aheadMs` after what sample() showed last (call sample() first):
    // the received path while the buffer has it, then the same prediction as during a loss burst.
    MotionSample predict(float aheadMs) const {
        if (samples_.empty()) return {};
        if (!playing_) return samples_.back();
        double when = playTime_ + aheadMs;
        uint32_t t = refTime_ + static_cast<uint32_t>(static_cast<int32_t>(std::floor(when)));
        MotionSample out = evaluate(t, static_cast<float>(when - std::floor(when)));
        out.pos = out.pos + offset_;
        return out;
    }

private:
    static constexpr uint32_t kWindow = 40;
    static constexpr int32_t kGapMs = 3000;
    static constexpr float kCoastSec = 0.35f;     // a gap keeps full speed this long ...
    static constexpr float kStopSec = 0.45f;      // ... then slows to a stop over this
    static constexpr float kGlideSec = 0.2f;      // time constant of the glide back onto real data
    static constexpr float kMaxGlide = 6.0f;      // a bigger jump is a real one (teleport): no glide
    static constexpr uint32_t kTurnBaseMs = 180;
    static constexpr float kTurnMinSpeed = 2.0f;  // m/s; slower movement gives no reliable turn rate
    static constexpr float kMaxTurn = 2.0f;       // rad/s

    void updateTargetDelay() {
        if (transitCount_ == 0) return;
        lowTransit_ = windowLow_;
        float jitter = static_cast<float>(windowHigh_ - windowLow_);
        float interval = interval_ > 0 ? interval_ : 50.0f;
        float target = interval * 1.1f + jitter + 8.0f;
        target = std::clamp(target, static_cast<float>(minDelay_), static_cast<float>(maxDelay_));
        // Grow quickly when the network gets worse, shrink slowly when it recovers.
        targetDelay_ = target > targetDelay_ ? target : targetDelay_ + (target - targetDelay_) * 0.02f;
    }

    MotionSample evaluate(uint32_t t, float frac) const {
        const MotionSample& first = samples_.front();
        if (static_cast<int32_t>(t - first.timeMs) < 0) return first;
        const size_t n = samples_.size();
        if (static_cast<int32_t>(t - samples_[n - 1].timeMs) >= 0) return extrapolate(t, frac);
        // Playback runs close behind the newest sample: search from the back.
        for (size_t i = n - 1; i >= 1; --i) {
            const MotionSample& a = samples_[i - 1];
            if (static_cast<int32_t>(t - a.timeMs) < 0) continue;
            float span = static_cast<float>(samples_[i].timeMs - a.timeMs);
            float k = std::clamp((static_cast<float>(t - a.timeMs) + frac) / span, 0.0f, 1.0f);
            return interpolate(i, k, span);
        }
        return first;
    }

    // Past the newest sample: keep going along the current curve (same speed, same rate of turn),
    // then slow down to a stop, as a guess of what the sender keeps doing during a gap.
    MotionSample extrapolate(uint32_t t, float frac) const {
        const MotionSample& last = samples_.back();
        MotionSample out = last;
        out.timeMs = t;
        float tau = (static_cast<float>(t - last.timeMs) + frac) / 1000.0f;
        float travel, keep;  // seconds of travel at full speed, and the speed left
        if (tau <= kCoastSec) {
            travel = tau;
            keep = 1.0f;
        } else if (tau <= kCoastSec + kStopSec) {
            float x = tau - kCoastSec;
            travel = kCoastSec + x - x * x / (2.0f * kStopSec);
            keep = 1.0f - x / kStopSec;
        } else {
            travel = kCoastSec + kStopSec * 0.5f;
            keep = 0.0f;
        }
        Vec3 v = tangent(samples_.size() - 1);
        float w = turnRate();
        float angle = w * travel;
        Vec3 flat{v.x, v.y, 0}, left{-v.y, v.x, 0};
        Vec3 d;
        if (std::fabs(angle) < 1e-3f) {
            d = flat * travel + left * (0.5f * w * travel * travel);
        } else {
            d = flat * (std::sin(angle) / w) + left * ((1.0f - std::cos(angle)) / w);
        }
        d.z = v.z * travel;
        out.pos = last.pos + d;
        float c = std::cos(angle), s = std::sin(angle);
        out.vel = Vec3{v.x * c - v.y * s, v.x * s + v.y * c, v.z} * keep;
        out.hasVel = true;
        out.heading = wrapHeading(last.heading + angle * 57.2957795f);
        return out;
    }

    // Rate of turn of the horizontal velocity at the newest sample (rad/s, counter-clockwise like
    // headings), measured over at least kTurnBaseMs so quantised velocities do not make it noisy.
    float turnRate() const {
        const size_t n = samples_.size();
        if (n < 2) return 0;
        const MotionSample& last = samples_[n - 1];
        for (size_t j = n - 1; j-- > 0;) {
            uint32_t span = last.timeMs - samples_[j].timeMs;
            if (span < kTurnBaseMs) continue;
            if (span > 4 * kTurnBaseMs) return 0;
            Vec3 a = tangent(j), b = tangent(n - 1);
            if (std::hypot(a.x, a.y) < kTurnMinSpeed || std::hypot(b.x, b.y) < kTurnMinSpeed) return 0;
            float angle = std::atan2(a.x * b.y - a.y * b.x, a.x * b.x + a.y * b.y);
            return std::clamp(angle / (span / 1000.0f), -kMaxTurn, kMaxTurn);
        }
        return 0;
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
    int32_t windowLow_ = 0, windowHigh_ = 0;  // extremes of transits_
    float interval_ = 0;
    float targetDelay_ = 0;
    uint32_t minDelay_, maxDelay_;
    uint32_t refTime_ = 0;  // sender time playback is measured from (first sample after a reset)
    double playTime_ = 0;   // ms since refTime_ shown at the last sample() call
    uint32_t lastLocal_ = 0;
    bool playing_ = false;
    // recovery after a gap
    MotionSample lastOut_;
    Vec3 offset_;
    uint32_t lastNewest_ = 0;
    bool haveOut_ = false, wasGuessing_ = false;
    // samples far behind the newest one (see push)
    uint32_t stale_ = 0, staleLast_ = 0;
};

}  // namespace coop
