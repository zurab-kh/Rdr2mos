// Snapshot interpolation: remote entities are rendered slightly in the past, between two
// received samples, so movement stays smooth despite network jitter and low send rates.
#pragma once
#include <cstdint>
#include <deque>
#include "mathx.h"

namespace coop {

struct MotionSample {
    uint32_t timeMs = 0;  // sender clock
    Vec3 pos;
    float heading = 0;
    float speed = 0;
};

class MotionBuffer {
public:
    explicit MotionBuffer(uint32_t delayMs = 120) : delayMs_(delayMs) {}

    void push(const MotionSample& s, uint32_t localNowMs) {
        if (!samples_.empty() && static_cast<int32_t>(s.timeMs - samples_.back().timeMs) <= 0) return;  // stale
        // Track (local clock - sender clock). Follow it slowly, but snap if it jumps (peer restarted).
        int32_t offset = static_cast<int32_t>(localNowMs - s.timeMs);
        if (samples_.empty() || offset - offset_ > 1000 || offset_ - offset > 1000) offset_ = offset;
        else offset_ += (offset - offset_) / 8;
        samples_.push_back(s);
        while (samples_.size() > 32) samples_.pop_front();
    }

    bool empty() const { return samples_.empty(); }
    const MotionSample& latest() const { return samples_.back(); }
    void clear() { samples_.clear(); }

    // Where the entity should be shown at local time `localNowMs`.
    MotionSample sample(uint32_t localNowMs) const {
        if (samples_.empty()) return {};
        uint32_t t = localNowMs - static_cast<uint32_t>(offset_) - delayMs_;
        if (static_cast<int32_t>(t - samples_.front().timeMs) <= 0) return samples_.front();
        for (size_t i = 1; i < samples_.size(); ++i) {
            const MotionSample& a = samples_[i - 1];
            const MotionSample& b = samples_[i];
            if (static_cast<int32_t>(t - b.timeMs) <= 0) {
                float span = static_cast<float>(b.timeMs - a.timeMs);
                float k = span > 0 ? static_cast<float>(t - a.timeMs) / span : 1.0f;
                MotionSample out;
                out.timeMs = t;
                out.pos = lerp(a.pos, b.pos, k);
                out.heading = lerpHeading(a.heading, b.heading, k);
                out.speed = a.speed + (b.speed - a.speed) * k;
                return out;
            }
        }
        // Newer than everything we have: extrapolate briefly along the last segment, then hold.
        MotionSample out = samples_.back();
        if (samples_.size() >= 2) {
            const MotionSample& a = samples_[samples_.size() - 2];
            const MotionSample& b = samples_.back();
            float span = static_cast<float>(b.timeMs - a.timeMs);
            float ahead = static_cast<float>(t - b.timeMs);
            if (ahead > 250.0f) ahead = 250.0f;
            if (span > 0) out.pos = b.pos + (b.pos - a.pos) * (ahead / span);
        }
        return out;
    }

private:
    std::deque<MotionSample> samples_;
    int32_t offset_ = 0;
    uint32_t delayMs_;
};

}  // namespace coop
