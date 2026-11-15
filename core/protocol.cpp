#include "protocol.h"

#include <algorithm>
#include <cmath>

namespace coop {

namespace {
constexpr float kPosStep = 0.02f;   // metres per unit, relative positions
constexpr float kVelStep = 0.25f;   // m/s per unit, entity velocities
constexpr float kPlayerVelStep = 0.01f;
constexpr float kAimStep = 0.05f;
constexpr float kTiltStep = 1.5f;   // degrees per unit

int32_t quantize(float v, float step) { return static_cast<int32_t>(std::lround(v / step)); }
int16_t q16(float v, float step) { return static_cast<int16_t>(std::clamp(quantize(v, step), -32767, 32767)); }
int8_t q8(float v, float step) { return static_cast<int8_t>(std::clamp(quantize(v, step), -127, 127)); }
bool fits16(float v, float step) { int32_t q = quantize(v, step); return q >= -32767 && q <= 32767; }

uint16_t packHeading(float degrees) {
    float h = std::fmod(degrees, 360.0f);
    if (h < 0) h += 360.0f;
    return static_cast<uint16_t>(static_cast<uint32_t>(std::lround(h / 360.0f * 65536.0f)) & 0xFFFF);
}
float unpackHeading(uint16_t v) { return v / 65536.0f * 360.0f; }

bool relativeFits(const Vec3& p, const Vec3& anchor) {
    return fits16(p.x - anchor.x, kPosStep) && fits16(p.y - anchor.y, kPosStep) && fits16(p.z - anchor.z, kPosStep);
}
}  // namespace

void writePacketHeader(ByteWriter& w, PacketType type) {
    w.u16(kMagic);
    w.u8(kProtocolVersion);
    w.u8(static_cast<uint8_t>(type));
}

bool readPacketHeader(ByteReader& r, PacketType& type, bool& versionOk) {
    uint16_t magic = r.u16();
    uint8_t version = r.u8();
    type = static_cast<PacketType>(r.u8());
    if (!r.ok() || magic != kMagic) return false;
    versionOk = version == kProtocolVersion;
    return true;
}

void write(ByteWriter& w, const HelloInfo& v) {
    w.u8(static_cast<uint8_t>(v.role));
    w.u32(v.nonce);
    w.str(v.name);
    w.str(v.avatar);
}
bool read(ByteReader& r, HelloInfo& v) {
    uint8_t role = r.u8();
    v.nonce = r.u32();
    v.name = r.str();
    v.avatar = r.str();
    if (!r.ok() || role > 1) return false;
    v.role = static_cast<Role>(role);
    return true;
}

// ---------------------------------------------------------------- player state (~40 bytes)
void write(ByteWriter& w, const PlayerState& v) {
    w.u32(v.timeMs);
    w.vec3(v.pos);
    w.i16(q16(v.vel.x, kPlayerVelStep));
    w.i16(q16(v.vel.y, kPlayerVelStep));
    w.i16(q16(v.vel.z, kPlayerVelStep));
    w.u16(packHeading(v.heading));
    w.i16(q16(v.aim.x - v.pos.x, kAimStep));
    w.i16(q16(v.aim.y - v.pos.y, kAimStep));
    w.i16(q16(v.aim.z - v.pos.z, kAimStep));
    w.u16(v.health);
    w.u16(v.maxHealth);
    w.u32(v.weapon);
    w.u16(v.flags);
    if (v.flags & PF_OnMount) {
        w.u16(v.mountRef);
        w.u32(v.mountModel);
    }
    if (v.flags & PF_InVehicle) {
        w.u16(v.vehicleRef);
        w.u32(v.vehicleModel);
        w.u8(static_cast<uint8_t>(v.seat));
    }
}
bool read(ByteReader& r, PlayerState& v) {
    v.timeMs = r.u32();
    v.pos = r.vec3();
    v.vel.x = r.i16() * kPlayerVelStep;
    v.vel.y = r.i16() * kPlayerVelStep;
    v.vel.z = r.i16() * kPlayerVelStep;
    v.heading = unpackHeading(r.u16());
    v.aim.x = v.pos.x + r.i16() * kAimStep;
    v.aim.y = v.pos.y + r.i16() * kAimStep;
    v.aim.z = v.pos.z + r.i16() * kAimStep;
    v.health = r.u16();
    v.maxHealth = r.u16();
    v.weapon = r.u32();
    v.flags = r.u16();
    v.mountRef = kRefNone;
    v.mountModel = 0;
    v.vehicleRef = kRefNone;
    v.vehicleModel = 0;
    v.seat = -1;
    if (v.flags & PF_OnMount) {
        v.mountRef = r.u16();
        v.mountModel = r.u32();
    }
    if (v.flags & PF_InVehicle) {
        v.vehicleRef = r.u16();
        v.vehicleModel = r.u32();
        v.seat = static_cast<int8_t>(r.u8());
    }
    return r.ok() && std::isfinite(v.pos.x) && std::isfinite(v.pos.y) && std::isfinite(v.pos.z);
}

// ---------------------------------------------------------------- entities
size_t encodedSize(const EntityState& e, const Vec3& anchor) {
    size_t n = 3;  // id + field mask
    if (e.fields & EF_Static) n += 9;
    if (e.fields & EF_Pos) n += relativeFits(e.pos, anchor) ? 6 : 12;
    if (e.fields & EF_Heading) n += 2;
    if (e.fields & EF_Tilt) n += 2;
    if (e.fields & EF_Vel) n += 3;
    if (e.fields & EF_Health) n += 2;
    if (e.fields & EF_State) n += 12;
    return n;
}

static void writeEntity(ByteWriter& w, const EntityState& e, const Vec3& anchor) {
    uint8_t fields = e.fields & static_cast<uint8_t>(~EF_FarPos);
    bool far = (fields & EF_Pos) && !relativeFits(e.pos, anchor);
    if (far) fields |= EF_FarPos;
    w.u16(e.id);
    w.u8(fields);
    if (fields & EF_Static) {
        w.u8(static_cast<uint8_t>(e.kind));
        w.u32(e.model);
        w.u32(e.outfit);
    }
    if (fields & EF_Pos) {
        if (far) {
            w.vec3(e.pos);
        } else {
            w.i16(q16(e.pos.x - anchor.x, kPosStep));
            w.i16(q16(e.pos.y - anchor.y, kPosStep));
            w.i16(q16(e.pos.z - anchor.z, kPosStep));
        }
    }
    if (fields & EF_Heading) w.u16(packHeading(e.heading));
    if (fields & EF_Tilt) {
        w.u8(static_cast<uint8_t>(q8(e.pitch, kTiltStep)));
        w.u8(static_cast<uint8_t>(q8(e.roll, kTiltStep)));
    }
    if (fields & EF_Vel) {
        w.u8(static_cast<uint8_t>(q8(e.vel.x, kVelStep)));
        w.u8(static_cast<uint8_t>(q8(e.vel.y, kVelStep)));
        w.u8(static_cast<uint8_t>(q8(e.vel.z, kVelStep)));
    }
    if (fields & EF_Health) w.i16(e.health);
    if (fields & EF_State) {
        w.u8(e.flags);
        w.u32(e.weapon);
        w.u16(e.mountRef);
        w.u16(e.vehicleRef);
        w.u8(static_cast<uint8_t>(e.seat));
        w.u16(e.targetRef);
    }
}

static bool readEntity(ByteReader& r, EntityState& e, const Vec3& anchor) {
    e = EntityState{};
    e.id = r.u16();
    uint8_t fields = r.u8();
    if (fields & EF_Static) {
        uint8_t kind = r.u8();
        if (kind > 1) return false;
        e.kind = static_cast<EntityKind>(kind);
        e.model = r.u32();
        e.outfit = r.u32();
    }
    if (fields & EF_Pos) {
        if (fields & EF_FarPos) {
            e.pos = r.vec3();
        } else {
            e.pos.x = anchor.x + r.i16() * kPosStep;
            e.pos.y = anchor.y + r.i16() * kPosStep;
            e.pos.z = anchor.z + r.i16() * kPosStep;
        }
    }
    if (fields & EF_Heading) e.heading = unpackHeading(r.u16());
    if (fields & EF_Tilt) {
        e.pitch = static_cast<int8_t>(r.u8()) * kTiltStep;
        e.roll = static_cast<int8_t>(r.u8()) * kTiltStep;
    }
    if (fields & EF_Vel) {
        e.vel.x = static_cast<int8_t>(r.u8()) * kVelStep;
        e.vel.y = static_cast<int8_t>(r.u8()) * kVelStep;
        e.vel.z = static_cast<int8_t>(r.u8()) * kVelStep;
    }
    if (fields & EF_Health) e.health = r.i16();
    if (fields & EF_State) {
        e.flags = r.u8();
        e.weapon = r.u32();
        e.mountRef = r.u16();
        e.vehicleRef = r.u16();
        e.seat = static_cast<int8_t>(r.u8());
        e.targetRef = r.u16();
    }
    e.fields = fields & static_cast<uint8_t>(~EF_FarPos);
    return r.ok() && e.id != 0 && e.id <= kMaxEntityId && std::isfinite(e.pos.x) && std::isfinite(e.pos.y) &&
           std::isfinite(e.pos.z);
}

void write(ByteWriter& w, const EntitySnapshot& v) {
    w.u32(v.timeMs);
    w.vec3(v.anchor);
    size_t n = std::min<size_t>(v.entities.size(), 255);
    w.u8(static_cast<uint8_t>(n));
    for (size_t i = 0; i < n; ++i) writeEntity(w, v.entities[i], v.anchor);
    size_t m = std::min<size_t>(v.removed.size(), 255);
    w.u8(static_cast<uint8_t>(m));
    for (size_t i = 0; i < m; ++i) w.u16(v.removed[i]);
}

bool read(ByteReader& r, EntitySnapshot& v) {
    v.timeMs = r.u32();
    v.anchor = r.vec3();
    size_t n = r.u8();
    if (!r.ok() || !std::isfinite(v.anchor.x) || !std::isfinite(v.anchor.y) || !std::isfinite(v.anchor.z)) return false;
    v.entities.resize(n);
    for (auto& e : v.entities)
        if (!readEntity(r, e, v.anchor)) return false;
    size_t m = r.u8();
    if (!r.ok() || r.remaining() < m * 2) return false;
    v.removed.resize(m);
    for (auto& id : v.removed) id = r.u16();
    return r.ok();
}

void encodeSnapshots(uint32_t timeMs, const Vec3& anchor, const std::vector<EntityState>& entities,
                     const std::vector<uint16_t>& removed, size_t maxBytes, std::vector<std::vector<uint8_t>>& out) {
    constexpr size_t kHeader = 4 + 12 + 1 + 1;
    EntitySnapshot snap;
    snap.timeMs = timeMs;
    snap.anchor = anchor;
    size_t size = kHeader;
    size_t nextRemoved = 0;
    auto emit = [&] {
        // Removals ride along in whatever room is left.
        while (nextRemoved < removed.size() && snap.removed.size() < 255 && size + 2 <= maxBytes) {
            snap.removed.push_back(removed[nextRemoved++]);
            size += 2;
        }
        if (snap.entities.empty() && snap.removed.empty()) return;
        out.push_back(encode(snap));
        snap.entities.clear();
        snap.removed.clear();
        size = kHeader;
    };
    for (const EntityState& e : entities) {
        if (e.fields == 0) continue;
        size_t need = encodedSize(e, anchor);
        if (!snap.entities.empty() && (size + need > maxBytes || snap.entities.size() == 255)) emit();
        snap.entities.push_back(e);
        size += need;
    }
    emit();
    while (nextRemoved < removed.size()) emit();
}

void write(ByteWriter& w, const Damage& v) {
    w.u16(v.target);
    w.u16(v.amount);
}
bool read(ByteReader& r, Damage& v) {
    v.target = r.u16();
    v.amount = r.u16();
    return r.ok();
}

void write(ByteWriter& w, const Appearance& v) { w.u32(v.outfit); }
bool read(ByteReader& r, Appearance& v) {
    v.outfit = r.u32();
    return r.ok();
}

void write(ByteWriter& w, const WorldInfo& v) {
    w.u8(v.hour);
    w.u8(v.minute);
    w.u8(v.second);
    w.u8(v.worldSync);
    w.u32(v.weather);
}
bool read(ByteReader& r, WorldInfo& v) {
    v.hour = r.u8();
    v.minute = r.u8();
    v.second = r.u8();
    v.worldSync = r.u8();
    v.weather = r.u32();
    return r.ok() && v.hour < 24 && v.minute < 60 && v.second < 60;
}

}  // namespace coop
