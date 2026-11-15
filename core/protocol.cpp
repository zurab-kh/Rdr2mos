#include "protocol.h"
#include <algorithm>
#include <cmath>

namespace coop {

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

void write(ByteWriter& w, const PlayerState& v) {
    w.u32(v.timeMs);
    w.vec3(v.pos);
    w.vec3(v.vel);
    w.f32(v.heading);
    w.vec3(v.aim);
    w.u16(v.health);
    w.u16(v.maxHealth);
    w.u32(v.weapon);
    w.u32(v.mountModel);
    w.u16(v.flags);
}
bool read(ByteReader& r, PlayerState& v) {
    v.timeMs = r.u32();
    v.pos = r.vec3();
    v.vel = r.vec3();
    v.heading = r.f32();
    v.aim = r.vec3();
    v.health = r.u16();
    v.maxHealth = r.u16();
    v.weapon = r.u32();
    v.mountModel = r.u32();
    v.flags = r.u16();
    return r.ok();
}

static void writeNpc(ByteWriter& w, const NpcState& n) {
    w.u16(n.id);
    w.u32(n.model);
    w.vec3(n.pos);
    w.u16(static_cast<uint16_t>(std::lround(std::fmod(n.heading + 360.0f, 360.0f) / 360.0f * 65535.0f)));
    w.u8(static_cast<uint8_t>(std::clamp(std::lround(n.speed * 10.0f), 0L, 255L)));
    w.i16(n.health);
    w.u8(n.flags);
    w.u16(n.mountId);
    w.u32(n.weapon);
}
static void readNpc(ByteReader& r, NpcState& n) {
    n.id = r.u16();
    n.model = r.u32();
    n.pos = r.vec3();
    n.heading = r.u16() / 65535.0f * 360.0f;
    n.speed = r.u8() / 10.0f;
    n.health = r.i16();
    n.flags = r.u8();
    n.mountId = r.u16();
    n.weapon = r.u32();
}

void write(ByteWriter& w, const WorldSnapshot& v) {
    w.u32(v.timeMs);
    w.u8(static_cast<uint8_t>(std::min(v.npcs.size(), kMaxNpcsPerPacket)));
    for (size_t i = 0; i < v.npcs.size() && i < kMaxNpcsPerPacket; ++i) writeNpc(w, v.npcs[i]);
}
bool read(ByteReader& r, WorldSnapshot& v) {
    v.timeMs = r.u32();
    size_t n = r.u8();
    if (!r.ok() || n > kMaxNpcsPerPacket || r.remaining() < n * kNpcWireSize) return false;
    v.npcs.resize(n);
    for (auto& npc : v.npcs) readNpc(r, npc);
    return r.ok();
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

void write(ByteWriter& w, const WorldInfo& v) {
    w.u8(v.hour);
    w.u8(v.minute);
    w.u8(v.second);
    w.u8(v.worldSync);
}
bool read(ByteReader& r, WorldInfo& v) {
    v.hour = r.u8();
    v.minute = r.u8();
    v.second = r.u8();
    v.worldSync = r.u8();
    return r.ok() && v.hour < 24 && v.minute < 60 && v.second < 60;
}

}  // namespace coop
