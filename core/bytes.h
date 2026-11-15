// Little-endian byte stream helpers used by the wire protocol.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace coop {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

class ByteWriter {
public:
    std::vector<uint8_t> data;

    void u8(uint8_t v) { data.push_back(v); }
    void u16(uint16_t v) { raw(&v, 2); }
    void u32(uint32_t v) { raw(&v, 4); }
    void i16(int16_t v) { raw(&v, 2); }
    void f32(float v) { raw(&v, 4); }
    void vec3(const Vec3& v) { f32(v.x); f32(v.y); f32(v.z); }
    void str(const std::string& s) {
        size_t n = s.size() > 255 ? 255 : s.size();
        u8(static_cast<uint8_t>(n));
        bytes(s.data(), n);
    }
    void bytes(const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        data.insert(data.end(), b, b + n);
    }

private:
    // The protocol is little-endian; every supported target (x64) is too.
    void raw(const void* p, size_t n) { bytes(p, n); }
};

class ByteReader {
public:
    ByteReader(const uint8_t* p, size_t n) : p_(p), end_(p + n) {}

    bool ok() const { return ok_; }
    size_t remaining() const { return static_cast<size_t>(end_ - p_); }

    uint8_t u8() { uint8_t v = 0; raw(&v, 1); return v; }
    uint16_t u16() { uint16_t v = 0; raw(&v, 2); return v; }
    uint32_t u32() { uint32_t v = 0; raw(&v, 4); return v; }
    int16_t i16() { int16_t v = 0; raw(&v, 2); return v; }
    float f32() { float v = 0; raw(&v, 4); return v; }
    Vec3 vec3() { Vec3 v; v.x = f32(); v.y = f32(); v.z = f32(); return v; }
    std::string str() {
        size_t n = u8();
        if (n > remaining()) { ok_ = false; return {}; }
        std::string s(reinterpret_cast<const char*>(p_), n);
        p_ += n;
        return s;
    }
    // Returns a pointer to n bytes inside the buffer, or nullptr on underflow.
    const uint8_t* take(size_t n) {
        if (n > remaining()) { ok_ = false; return nullptr; }
        const uint8_t* r = p_;
        p_ += n;
        return r;
    }

private:
    void raw(void* out, size_t n) {
        if (n > remaining()) { ok_ = false; p_ = end_; return; }
        std::memcpy(out, p_, n);
        p_ += n;
    }
    const uint8_t* p_;
    const uint8_t* end_;
    bool ok_ = true;
};

}  // namespace coop
