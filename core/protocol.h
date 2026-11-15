// Wire protocol shared by the game plugin, the relay server and the tests.
//
// Bandwidth matters more than CPU here, so world state is sent as compact, field-masked
// entity updates: positions as 2 cm steps relative to a per-packet anchor, headings as
// 16-bit angles, velocities as 0.25 m/s steps, and only the fields that changed.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "bytes.h"

namespace coop {

constexpr uint16_t kMagic = 0x4352;  // "RC"
constexpr uint8_t kProtocolVersion = 4;
constexpr uint16_t kDefaultPort = 27960;
constexpr size_t kMaxDatagram = 1200;  // stays below any sane path MTU
constexpr size_t kMaxMessage = 1100;   // payload budget for one unreliable message

enum class PacketType : uint8_t {
    Hello = 1,
    Data = 2,
    Bye = 3,
    RelayRegister = 10,
    RelayStatus = 11,
};

enum class Role : uint8_t {
    Host = 0,   // owns the story world: missions, NPCs, vehicles, clock, weather
    Guest = 1,  // joins the host's world
};

enum class Msg : uint8_t {
    PlayerState = 1,
    EntitySnapshot = 2,
    Damage = 3,
    WorldInfo = 4,
    Ping = 5,
    Pong = 6,
    Appearance = 7,
    Story = 8,          // mission identity, deaths (reliable)
    Camera = 9,         // rendered camera during the host's scenes (host-world mode)
    Waypoint = 10,      // map waypoint (reliable)
    SaveRequest = 11,   // guest asks for the host's save (reliable)
    SaveChunk = 12,     // a piece of the save file (reliable)
};

// How the two games share the story.
enum class StoryMode : uint8_t {
    HostWorld = 0,  // the guest visits the host's world; only the host runs missions
    Shared = 1,     // both play the same save: each runs the mission, the games are kept in step
};

// References to things in the host's world. Entity ids are assigned by the host.
constexpr uint16_t kRefNone = 0;
constexpr uint16_t kRefHostPlayer = 0xFFFE;
constexpr uint16_t kRefGuestPlayer = 0xFFFD;
constexpr uint16_t kMaxEntityId = 0xFFF0;

struct HelloInfo {
    Role role = Role::Host;
    uint32_t nonce = 0;        // changes on every (re)start so a restarted peer is noticed
    std::string name;
    std::string avatar;        // ped model name the peer wants to be shown as
    StoryMode storyMode = StoryMode::Shared;
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
    PF_Cutscene = 1 << 8,      // in a scripted scene: the player has no control
    PF_Holding = 1 << 9,       // finished a scene first and waits for the friend (world frozen)
};

struct PlayerState {
    uint32_t timeMs = 0;       // sender clock
    Vec3 pos;                  // the mount's / vehicle's position while riding or driving
    Vec3 vel;
    float heading = 0;
    Vec3 aim;                  // world point the player is aiming at
    uint16_t health = 0;
    uint16_t maxHealth = 0;
    uint32_t weapon = 0;
    uint16_t flags = 0;
    // PF_OnMount: a host-world entity id, or kRefNone + model for the player's own horse.
    uint16_t mountRef = kRefNone;
    uint32_t mountModel = 0;
    // PF_InVehicle: same idea; seat -1 is the driver.
    uint16_t vehicleRef = kRefNone;
    uint32_t vehicleModel = 0;
    int8_t seat = -1;
};

enum class EntityKind : uint8_t { Ped = 0, Vehicle = 1 };

enum EntityFields : uint8_t {
    EF_Pos = 1 << 0,
    EF_Heading = 1 << 1,
    EF_Tilt = 1 << 2,      // pitch + roll (vehicles)
    EF_Vel = 1 << 3,
    EF_Health = 1 << 4,
    EF_State = 1 << 5,     // flags, weapon, mount/vehicle/target references
    EF_Static = 1 << 6,    // kind + model
    EF_FarPos = 1 << 7,    // wire only: position sent as full floats
};

enum EntityFlags : uint8_t {
    NF_Dead = 1 << 0,
    NF_Shooting = 1 << 1,
    NF_InCombat = 1 << 2,
    NF_Ragdoll = 1 << 3,
    NF_Aiming = 1 << 4,
};

struct EntityState {
    uint16_t id = 0;           // host-assigned, 1..kMaxEntityId
    uint8_t fields = 0;        // which of the groups below are meaningful
    // EF_Static
    EntityKind kind = EntityKind::Ped;
    uint32_t model = 0;
    uint32_t outfit = 0;       // meta ped outfit hash, 0 = unknown
    uint32_t script = 0;       // hash of the script that owns it (mission entities), 0 = ambient
    // EF_Pos / EF_Heading / EF_Tilt / EF_Vel
    Vec3 pos;
    float heading = 0;
    float pitch = 0, roll = 0;
    Vec3 vel;
    // EF_Health
    int16_t health = 0;
    // EF_State
    uint8_t flags = 0;
    uint32_t weapon = 0;
    uint16_t mountRef = kRefNone;
    uint16_t vehicleRef = kRefNone;
    int8_t seat = -1;
    uint16_t targetRef = kRefNone;
};

struct EntitySnapshot {
    uint32_t timeMs = 0;
    Vec3 anchor;                       // positions are encoded relative to this point
    std::vector<EntityState> entities;
    std::vector<uint16_t> removed;     // ids that no longer exist / left the streamed area
};

struct Damage {
    uint16_t target = kRefNone;        // host-world entity id (guest -> host), or a player ref
    uint16_t amount = 0;
};

// The look of a player's character; sent reliably when it changes.
struct Appearance {
    uint32_t outfit = 0;
};

enum class StoryEvent : uint8_t {
    Mission = 1,    // the mission being played changed (script hash, 0 = free roam)
    Died = 2,       // the player died during a mission
};

struct StoryInfo {
    StoryEvent event = StoryEvent::Mission;
    uint32_t missionScript = 0;
};

struct CameraState {
    uint32_t timeMs = 0;
    Vec3 pos;
    Vec3 rot;      // pitch, roll, yaw in degrees
    float fov = 50.0f;
};

struct WaypointInfo {
    uint8_t active = 0;
    float x = 0, y = 0;
};

struct SaveRequest {
    uint8_t reserved = 0;
};

struct SaveChunk {
    uint32_t total = 0;    // size of the whole file
    uint32_t crc = 0;      // CRC-32 of the whole file
    uint32_t offset = 0;
    std::vector<uint8_t> data;
};
constexpr size_t kSaveChunkBytes = 960;
constexpr uint32_t kMaxSaveBytes = 64u << 20;

struct WorldInfo {
    uint8_t hour = 0, minute = 0, second = 0;
    uint8_t worldSync = 0;     // host is streaming its world; the guest should show only that
    uint32_t weather = 0;      // weather type hash, 0 = unknown
};

void write(ByteWriter& w, const HelloInfo& v);
bool read(ByteReader& r, HelloInfo& v);
void write(ByteWriter& w, const PlayerState& v);
bool read(ByteReader& r, PlayerState& v);
void write(ByteWriter& w, const EntitySnapshot& v);
bool read(ByteReader& r, EntitySnapshot& v);
void write(ByteWriter& w, const Damage& v);
bool read(ByteReader& r, Damage& v);
void write(ByteWriter& w, const Appearance& v);
bool read(ByteReader& r, Appearance& v);
void write(ByteWriter& w, const StoryInfo& v);
bool read(ByteReader& r, StoryInfo& v);
void write(ByteWriter& w, const CameraState& v);
bool read(ByteReader& r, CameraState& v);
void write(ByteWriter& w, const WaypointInfo& v);
bool read(ByteReader& r, WaypointInfo& v);
void write(ByteWriter& w, const SaveRequest& v);
bool read(ByteReader& r, SaveRequest& v);
void write(ByteWriter& w, const SaveChunk& v);
bool read(ByteReader& r, SaveChunk& v);
uint32_t crc32(const uint8_t* data, size_t size, uint32_t crc = 0);
void write(ByteWriter& w, const WorldInfo& v);
bool read(ByteReader& r, WorldInfo& v);

// Bytes one entity takes on the wire with the given anchor.
size_t encodedSize(const EntityState& e, const Vec3& anchor);
// Splits entities (and removals) into as many snapshot messages as needed to stay under maxBytes each.
void encodeSnapshots(uint32_t timeMs, const Vec3& anchor, const std::vector<EntityState>& entities,
                     const std::vector<uint16_t>& removed, size_t maxBytes, std::vector<std::vector<uint8_t>>& out);

template <typename T>
std::vector<uint8_t> encode(const T& v) {
    ByteWriter w;
    write(w, v);
    return std::move(w.data);
}
template <typename T>
bool decode(const uint8_t* data, size_t size, T& v) {
    ByteReader r(data, size);
    return read(r, v) && r.remaining() == 0;
}
template <typename T>
bool decode(const std::vector<uint8_t>& bytes, T& v) {
    return decode(bytes.data(), bytes.size(), v);
}

// Every datagram starts with this 4-byte header.
void writePacketHeader(ByteWriter& w, PacketType type);
// Returns false for foreign traffic. `versionOk` is false when magic matches but the version differs.
bool readPacketHeader(ByteReader& r, PacketType& type, bool& versionOk);

}  // namespace coop
