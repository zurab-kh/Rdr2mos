#include "protocol.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace coop {

namespace {
constexpr float kPosStep = 0.02f;   // metres per unit, relative positions
constexpr float kVelStep = 0.25f;   // m/s per unit, entity velocities
constexpr float kPlayerVelStep = 0.01f;
constexpr float kAimStep = 0.05f;
constexpr float kTiltStep = 1.5f;   // degrees per unit

// Clamped before rounding: lround of a value outside `long` (or of NaN) gives an unspecified result.
int32_t quantize(float v, float step) {
    float q = v / step;
    if (!(q > -2.0e9f)) q = -2.0e9f;
    if (!(q < 2.0e9f)) q = 2.0e9f;
    return static_cast<int32_t>(std::lround(q));
}
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
    w.str(v.campaign);
    w.str(v.password);
    w.u8(v.wantsWorld);
    w.u32(v.cookie);
}
bool read(ByteReader& r, HelloInfo& v) {
    uint8_t role = r.u8();
    v.nonce = r.u32();
    v.name = r.str();
    v.avatar = r.str();
    v.campaign = r.str();
    v.password = r.str();
    v.wantsWorld = r.u8();
    v.cookie = r.u32();
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
    if (e.fields & EF_Static) n += 13;
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
        w.u32(e.script);
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
        e.script = r.u32();
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
    // Every entity takes at least 3 bytes: reject a count the datagram cannot hold before allocating.
    if (!r.ok() || r.remaining() < n * 3 + 1 || !std::isfinite(v.anchor.x) || !std::isfinite(v.anchor.y) ||
        !std::isfinite(v.anchor.z))
        return false;
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

void write(ByteWriter& w, const StoryInfo& v) {
    w.u8(static_cast<uint8_t>(v.event));
    w.u32(v.missionScript);
}
bool read(ByteReader& r, StoryInfo& v) {
    uint8_t e = r.u8();
    v.event = static_cast<StoryEvent>(e);
    v.missionScript = r.u32();
    return r.ok() && (e == 1 || e == 2);
}

void write(ByteWriter& w, const CameraState& v) {
    w.u32(v.timeMs);
    w.vec3(v.pos);
    w.i16(q16(v.rot.x, 0.01f));
    w.i16(q16(v.rot.y, 0.01f));
    w.u16(packHeading(v.rot.z));
    w.u16(static_cast<uint16_t>(std::clamp(std::lround(v.fov * 100.0f), 1L, 65535L)));
}
bool read(ByteReader& r, CameraState& v) {
    v.timeMs = r.u32();
    v.pos = r.vec3();
    v.rot.x = r.i16() * 0.01f;
    v.rot.y = r.i16() * 0.01f;
    v.rot.z = unpackHeading(r.u16());
    v.fov = r.u16() / 100.0f;
    return r.ok() && std::isfinite(v.pos.x) && std::isfinite(v.pos.y) && std::isfinite(v.pos.z) && v.fov >= 1.0f &&
           v.fov <= 170.0f;
}

void write(ByteWriter& w, const WaypointInfo& v) {
    w.u8(v.active);
    w.f32(v.x);
    w.f32(v.y);
}
bool read(ByteReader& r, WaypointInfo& v) {
    v.active = r.u8();
    v.x = r.f32();
    v.y = r.f32();
    return r.ok() && std::isfinite(v.x) && std::isfinite(v.y);
}

void write(ByteWriter& w, const SaveRequest& v) { w.u8(v.reserved); }
bool read(ByteReader& r, SaveRequest& v) {
    v.reserved = r.u8();
    return r.ok();
}

void write(ByteWriter& w, const SaveChunk& v) {
    w.u32(v.total);
    w.u32(v.crc);
    w.u32(v.offset);
    w.u16(static_cast<uint16_t>(v.data.size()));
    w.bytes(v.data.data(), v.data.size());
}
bool read(ByteReader& r, SaveChunk& v) {
    v.total = r.u32();
    v.crc = r.u32();
    v.offset = r.u32();
    size_t n = r.u16();
    const uint8_t* p = r.take(n);
    if (!r.ok() || n > kSaveChunkBytes || v.total > kMaxSaveBytes || v.offset > v.total || n > v.total - v.offset)
        return false;
    v.data.assign(p, p + n);
    return true;
}

namespace {
// Slicing-by-8 tables. A function-local static is initialised exactly once even when the built-in
// server thread and the game thread both check a save at the same moment.
struct Crc32Tables {
    uint32_t t[8][256];
    Crc32Tables() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; ++i)
            for (int s = 1; s < 8; ++s) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFF];
    }
};
const Crc32Tables& crcTables() {
    static const Crc32Tables tables;
    return tables;
}
}  // namespace

uint32_t crc32(const uint8_t* data, size_t size, uint32_t crc) {
    const auto& t = crcTables().t;
    crc = ~crc;
    // Eight bytes per step (little-endian loads, like the rest of the protocol).
    while (size >= 8) {
        uint32_t lo, hi;
        std::memcpy(&lo, data, 4);
        std::memcpy(&hi, data + 4, 4);
        lo ^= crc;
        crc = t[7][lo & 0xFF] ^ t[6][(lo >> 8) & 0xFF] ^ t[5][(lo >> 16) & 0xFF] ^ t[4][lo >> 24] ^
              t[3][hi & 0xFF] ^ t[2][(hi >> 8) & 0xFF] ^ t[1][(hi >> 16) & 0xFF] ^ t[0][hi >> 24];
        data += 8;
        size -= 8;
    }
    while (size--) crc = t[0][(crc ^ *data++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void write(ByteWriter& w, const Appearance& v) { w.u32(v.outfit); }
bool read(ByteReader& r, Appearance& v) {
    v.outfit = r.u32();
    return r.ok();
}

std::vector<uint8_t> withSource(uint8_t player, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out;
    out.reserve(payload.size() + 1);
    out.push_back(player);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

void write(ByteWriter& w, const Welcome& v) {
    w.u8(v.playerId);
    w.u8(static_cast<uint8_t>(v.storyMode));
    w.u8(v.hasCampaignSave);
    w.str(v.campaign);
}
bool read(ByteReader& r, Welcome& v) {
    v.playerId = r.u8();
    uint8_t mode = r.u8();
    v.hasCampaignSave = r.u8();
    v.campaign = r.str();
    if (!r.ok() || mode > 1 || v.playerId == 0 || v.playerId > kMaxPlayers) return false;
    v.storyMode = static_cast<StoryMode>(mode);
    return true;
}

void write(ByteWriter& w, const Roster& v) {
    w.u8(v.worldOwner);
    w.u16(v.worldEpoch);
    w.u8(static_cast<uint8_t>(std::min<size_t>(v.players.size(), kMaxPlayers)));
    for (size_t i = 0; i < v.players.size() && i < kMaxPlayers; ++i) {
        w.u8(v.players[i].id);
        w.str(v.players[i].name);
        w.str(v.players[i].avatar);
    }
}
bool read(ByteReader& r, Roster& v) {
    v.worldOwner = r.u8();
    v.worldEpoch = r.u16();
    size_t n = r.u8();
    if (!r.ok() || n > kMaxPlayers || r.remaining() < n * 3) return false;
    v.players.resize(n);
    uint64_t seen = 0;
    for (auto& p : v.players) {
        p.id = r.u8();
        p.name = r.str();
        p.avatar = r.str();
        if (p.id == 0 || p.id > kMaxPlayers || (seen >> p.id & 1)) return false;
        seen |= uint64_t(1) << p.id;
    }
    return r.ok();
}

void write(ByteWriter& w, const StoryCommandMsg& v) {
    w.u8(static_cast<uint8_t>(v.cmd));
    w.u8(v.flag);
    w.vec3(v.pos);
    w.f32(v.distance);
    w.str(v.text);
}
bool read(ByteReader& r, StoryCommandMsg& v) {
    uint8_t c = r.u8();
    v.cmd = static_cast<StoryCmd>(c);
    v.flag = r.u8();
    v.pos = r.vec3();
    v.distance = r.f32();
    v.text = r.str();
    return r.ok() && c >= 1 && c <= 6 && std::isfinite(v.pos.x) && std::isfinite(v.pos.y) && std::isfinite(v.pos.z) &&
           std::isfinite(v.distance);
}

void write(ByteWriter& w, const Denied& v) { w.str(v.reason); }
bool read(ByteReader& r, Denied& v) {
    v.reason = r.str();
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
