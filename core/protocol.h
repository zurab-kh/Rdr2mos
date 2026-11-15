// Wire protocol shared by the game plugin, the relay server and the tests.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "bytes.h"

namespace coop {

constexpr uint16_t kMagic = 0x4352;  // "RC"
constexpr uint8_t kProtocolVersion = 1;
constexpr uint16_t kDefaultPort = 27960;
constexpr size_t kMaxDatagram = 1200;  // stays below any sane path MTU

enum class PacketType : uint8_t {
    Hello = 1,
    Data = 2,
    Bye = 3,
    RelayRegister = 10,
    RelayStatus = 11,
};

enum class Role : uint8_t {
    Host = 0,   // owns the story world: missions, NPCs, clock
    Guest = 1,  // joins the host's world
};

enum class Msg : uint8_t {
    PlayerState = 1,
    WorldSnapshot = 2,
    Damage = 3,
    WorldInfo = 4,
    Ping = 5,
    Pong = 6,
};

struct HelloInfo {
    Role role = Role::Host;
    uint32_t nonce = 0;        // changes on every (re)start so a restarted peer is noticed
    std::string name;
    std::string avatar;        // ped model name the peer wants to be shown as
};

enum PlayerFlags : uint16_t {
    PF_OnMount = 1 << 0,
    PF_Aiming = 1 << 1,
    PF_Shooting = 1 << 2,
    PF_Dead = 1 << 3,
    PF_Ragdoll = 1 << 4,
    PF_Crouching = 1 << 5,
    PF_InVehicle = 1 << 6,
    PF_InMission = 1 << 7,
};

struct PlayerState {
    uint32_t timeMs = 0;       // sender clock
    Vec3 pos;                  // the mount's position while PF_OnMount is set
    Vec3 vel;
    float heading = 0;
    Vec3 aim;                  // world point the player is aiming at
    uint16_t health = 0;
    uint16_t maxHealth = 0;
    uint32_t weapon = 0;
    uint32_t mountModel = 0;
    uint16_t flags = 0;
};

enum NpcFlags : uint8_t {
    NF_Dead = 1 << 0,
    NF_Shooting = 1 << 1,
    NF_InCombat = 1 << 2,
    NF_TargetsGuest = 1 << 3,  // fighting the guest rather than the host
    NF_Ragdoll = 1 << 4,
};

struct NpcState {
    uint16_t id = 0;           // host-assigned, never 0
    uint32_t model = 0;
    Vec3 pos;
    float heading = 0;
    float speed = 0;
    int16_t health = 0;
    uint8_t flags = 0;
    uint16_t mountId = 0;      // id of the NPC this one rides, 0 if on foot
    uint32_t weapon = 0;
};
constexpr size_t kNpcWireSize = 30;
constexpr size_t kMaxNpcsPerPacket = 32;

struct WorldSnapshot {
    uint32_t timeMs = 0;
    std::vector<NpcState> npcs;
};

constexpr uint16_t kTargetPlayer = 0;  // Damage::target meaning "you, the peer player"

struct Damage {
    uint16_t target = kTargetPlayer;   // NPC id (guest -> host) or kTargetPlayer
    uint16_t amount = 0;
};

struct WorldInfo {
    uint8_t hour = 0, minute = 0, second = 0;
    uint8_t worldSync = 0;     // host is streaming its NPCs; the guest should show only those
};

void write(ByteWriter& w, const HelloInfo& v);
bool read(ByteReader& r, HelloInfo& v);
void write(ByteWriter& w, const PlayerState& v);
bool read(ByteReader& r, PlayerState& v);
void write(ByteWriter& w, const WorldSnapshot& v);
bool read(ByteReader& r, WorldSnapshot& v);
void write(ByteWriter& w, const Damage& v);
bool read(ByteReader& r, Damage& v);
void write(ByteWriter& w, const WorldInfo& v);
bool read(ByteReader& r, WorldInfo& v);

template <typename T>
std::vector<uint8_t> encode(const T& v) {
    ByteWriter w;
    write(w, v);
    return std::move(w.data);
}
template <typename T>
bool decode(const std::vector<uint8_t>& bytes, T& v) {
    ByteReader r(bytes.data(), bytes.size());
    return read(r, v);
}

// Every datagram starts with this 4-byte header.
void writePacketHeader(ByteWriter& w, PacketType type);
// Returns false for foreign traffic. `versionOk` is false when magic matches but the version differs.
bool readPacketHeader(ByteReader& r, PacketType& type, bool& versionOk);

}  // namespace coop
