#pragma once

// Wire-protocol packet format for the M11 networking tier.
//
// All packets begin with a 1-byte NetPacketKind followed by a kind-specific
// payload.  Numeric fields are stored little-endian.  No std:: types or
// engine types appear here — only fixed-width types from <cstdint> so this
// header can be included from both the engine and plugin translation units
// without pulling in heavy dependencies.
//
// The serialization helpers operate on std::vector<uint8_t> staging buffers
// (the same type ITransport::InboundPacket::data uses) for convenience in the
// tests; the helpers are header-only and entirely independent of ENet.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace net {

enum class NetPacketKind : uint8_t {
    EditIntent    = 1,  // client → server: propose a voxel write
    CommittedEdit = 2,  // server → client: apply this committed write
    NetMessage    = 3,  // either direction: plugin MessageEnvelope payload
    JoinResponse  = 4,  // server → client: world seed + layer config
    DirtyChunkData = 5, // server → client: raw chunk file bytes
    JoinComplete  = 6,  // server → client: handshake done
    ResyncRequest = 7,  // client → server: request full resync
};

// ---------------------------------------------------------------------------
// Primitive write helpers — append to a byte buffer
// ---------------------------------------------------------------------------

inline void write_u8(std::vector<uint8_t>& buf, uint8_t v)
{
    buf.push_back(v);
}

inline void write_u64(std::vector<uint8_t>& buf, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        buf.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

inline void write_u32(std::vector<uint8_t>& buf, uint32_t v)
{
    buf.push_back(static_cast<uint8_t>(v));
    buf.push_back(static_cast<uint8_t>(v >> 8));
    buf.push_back(static_cast<uint8_t>(v >> 16));
    buf.push_back(static_cast<uint8_t>(v >> 24));
}

inline void write_f32(std::vector<uint8_t>& buf, float v)
{
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    write_u32(buf, bits);
}

inline void write_f64(std::vector<uint8_t>& buf, double v)
{
    uint64_t bits;
    std::memcpy(&bits, &v, 8);
    buf.push_back(static_cast<uint8_t>(bits));
    buf.push_back(static_cast<uint8_t>(bits >> 8));
    buf.push_back(static_cast<uint8_t>(bits >> 16));
    buf.push_back(static_cast<uint8_t>(bits >> 24));
    buf.push_back(static_cast<uint8_t>(bits >> 32));
    buf.push_back(static_cast<uint8_t>(bits >> 40));
    buf.push_back(static_cast<uint8_t>(bits >> 48));
    buf.push_back(static_cast<uint8_t>(bits >> 56));
}

// ---------------------------------------------------------------------------
// Primitive read helpers — consume from a buffer at a given offset
// ---------------------------------------------------------------------------

inline uint64_t read_u64(const std::vector<uint8_t>& buf, size_t& off)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= (static_cast<uint64_t>(buf[off + i]) << (8 * i));
    off += 8;
    return v;
}

inline uint8_t read_u8(const std::vector<uint8_t>& buf, size_t& off)
{
    return buf[off++];
}

inline uint32_t read_u32(const std::vector<uint8_t>& buf, size_t& off)
{
    uint32_t v = static_cast<uint32_t>(buf[off])
               | (static_cast<uint32_t>(buf[off+1]) << 8)
               | (static_cast<uint32_t>(buf[off+2]) << 16)
               | (static_cast<uint32_t>(buf[off+3]) << 24);
    off += 4;
    return v;
}

inline float read_f32(const std::vector<uint8_t>& buf, size_t& off)
{
    uint32_t bits = read_u32(buf, off);
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
}

inline double read_f64(const std::vector<uint8_t>& buf, size_t& off)
{
    uint64_t bits = static_cast<uint64_t>(buf[off])
                  | (static_cast<uint64_t>(buf[off+1]) << 8)
                  | (static_cast<uint64_t>(buf[off+2]) << 16)
                  | (static_cast<uint64_t>(buf[off+3]) << 24)
                  | (static_cast<uint64_t>(buf[off+4]) << 32)
                  | (static_cast<uint64_t>(buf[off+5]) << 40)
                  | (static_cast<uint64_t>(buf[off+6]) << 48)
                  | (static_cast<uint64_t>(buf[off+7]) << 56);
    off += 8;
    double v;
    std::memcpy(&v, &bits, 8);
    return v;
}

// ---------------------------------------------------------------------------
// EditIntent  (client → server)
//
//   [kind:u8][x:f64][y:f64][z:f64][density:f32][structural_strength:f32]
//   [thermal_conductivity:f32][porosity:f32][hardness:f32]
//   [light_emission:f32][palette_index:u8][_pad:3×u8]
//
// Total: 1 + 3*8 + 6*4 + 1 + 3 = 53 bytes
// ---------------------------------------------------------------------------

struct EditIntentPayload {
    double  x, y, z;
    float   density, structural_strength, thermal_conductivity, porosity, hardness;
    float   light_emission;
    uint8_t palette_index;
};

inline std::vector<uint8_t> encode_edit_intent(const EditIntentPayload& p)
{
    std::vector<uint8_t> buf;
    buf.reserve(53);
    write_u8 (buf, static_cast<uint8_t>(NetPacketKind::EditIntent));
    write_f64(buf, p.x);
    write_f64(buf, p.y);
    write_f64(buf, p.z);
    write_f32(buf, p.density);
    write_f32(buf, p.structural_strength);
    write_f32(buf, p.thermal_conductivity);
    write_f32(buf, p.porosity);
    write_f32(buf, p.hardness);
    write_f32(buf, p.light_emission);
    write_u8 (buf, p.palette_index);
    write_u8 (buf, 0); write_u8(buf, 0); write_u8(buf, 0); // _pad
    return buf;
}

inline bool decode_edit_intent(const std::vector<uint8_t>& buf, EditIntentPayload& out)
{
    if (buf.size() < 53 || buf[0] != static_cast<uint8_t>(NetPacketKind::EditIntent))
        return false;
    size_t off = 1;
    out.x                    = read_f64(buf, off);
    out.y                    = read_f64(buf, off);
    out.z                    = read_f64(buf, off);
    out.density              = read_f32(buf, off);
    out.structural_strength  = read_f32(buf, off);
    out.thermal_conductivity = read_f32(buf, off);
    out.porosity             = read_f32(buf, off);
    out.hardness             = read_f32(buf, off);
    out.light_emission       = read_f32(buf, off);
    out.palette_index        = read_u8 (buf, off);
    return true;
}

// ---------------------------------------------------------------------------
// CommittedEdit  (server → client)
//
//   [kind:u8][seq:u32][source:u32][x:f64][y:f64][z:f64][density:f32]
//   [structural_strength:f32][thermal_conductivity:f32][porosity:f32]
//   [hardness:f32][light_emission:f32][palette_index:u8][_pad:3×u8]
//
// Total: 1 + 4 + 4 + 3*8 + 6*4 + 1 + 3 = 61 bytes
// ---------------------------------------------------------------------------

struct CommittedEditPayload {
    uint32_t seq;
    uint32_t source_player;
    double   x, y, z;
    float    density, structural_strength, thermal_conductivity, porosity, hardness;
    float    light_emission;
    uint8_t  palette_index;
};

inline std::vector<uint8_t> encode_committed_edit(const CommittedEditPayload& p)
{
    std::vector<uint8_t> buf;
    buf.reserve(61);
    write_u8 (buf, static_cast<uint8_t>(NetPacketKind::CommittedEdit));
    write_u32(buf, p.seq);
    write_u32(buf, p.source_player);
    write_f64(buf, p.x);
    write_f64(buf, p.y);
    write_f64(buf, p.z);
    write_f32(buf, p.density);
    write_f32(buf, p.structural_strength);
    write_f32(buf, p.thermal_conductivity);
    write_f32(buf, p.porosity);
    write_f32(buf, p.hardness);
    write_f32(buf, p.light_emission);
    write_u8 (buf, p.palette_index);
    write_u8 (buf, 0); write_u8(buf, 0); write_u8(buf, 0); // _pad
    return buf;
}

inline bool decode_committed_edit(const std::vector<uint8_t>& buf, CommittedEditPayload& out)
{
    if (buf.size() < 61 || buf[0] != static_cast<uint8_t>(NetPacketKind::CommittedEdit))
        return false;
    size_t off = 1;
    out.seq                    = read_u32(buf, off);
    out.source_player          = read_u32(buf, off);
    out.x                      = read_f64(buf, off);
    out.y                      = read_f64(buf, off);
    out.z                      = read_f64(buf, off);
    out.density                = read_f32(buf, off);
    out.structural_strength    = read_f32(buf, off);
    out.thermal_conductivity   = read_f32(buf, off);
    out.porosity               = read_f32(buf, off);
    out.hardness               = read_f32(buf, off);
    out.light_emission         = read_f32(buf, off);
    out.palette_index          = read_u8 (buf, off);
    return true;
}

// ---------------------------------------------------------------------------
// NetMessage  (either direction)
//
//   [kind:u8][sender:u32][target:u8][target_player:u32][reliability:u8]
//   [channel_len:u32][channel bytes][payload_len:u32][payload bytes]
// ---------------------------------------------------------------------------

struct NetMessagePayload {
    uint32_t           sender_id;
    uint8_t            target;         // MessageTarget as uint8_t
    uint32_t           target_player;
    uint8_t            reliability;    // MessageReliability as uint8_t
    std::string        channel_id;
    std::vector<uint8_t> payload;
};

inline std::vector<uint8_t> encode_net_message(const NetMessagePayload& p)
{
    std::vector<uint8_t> buf;
    write_u8 (buf, static_cast<uint8_t>(NetPacketKind::NetMessage));
    write_u32(buf, p.sender_id);
    write_u8 (buf, p.target);
    write_u32(buf, p.target_player);
    write_u8 (buf, p.reliability);
    write_u32(buf, static_cast<uint32_t>(p.channel_id.size()));
    for (char c : p.channel_id) buf.push_back(static_cast<uint8_t>(c));
    write_u32(buf, static_cast<uint32_t>(p.payload.size()));
    buf.insert(buf.end(), p.payload.begin(), p.payload.end());
    return buf;
}

inline bool decode_net_message(const std::vector<uint8_t>& buf, NetMessagePayload& out)
{
    if (buf.empty() || buf[0] != static_cast<uint8_t>(NetPacketKind::NetMessage))
        return false;
    size_t off = 1;
    if (off + 4 > buf.size()) return false;
    out.sender_id    = read_u32(buf, off);
    if (off + 1 > buf.size()) return false;
    out.target       = read_u8(buf, off);
    if (off + 4 > buf.size()) return false;
    out.target_player = read_u32(buf, off);
    if (off + 1 > buf.size()) return false;
    out.reliability  = read_u8(buf, off);
    if (off + 4 > buf.size()) return false;
    uint32_t ch_len = read_u32(buf, off);
    if (off + ch_len > buf.size()) return false;
    out.channel_id.assign(reinterpret_cast<const char*>(buf.data() + off), ch_len);
    off += ch_len;
    if (off + 4 > buf.size()) return false;
    uint32_t pay_len = read_u32(buf, off);
    if (off + pay_len > buf.size()) return false;
    out.payload.assign(buf.begin() + static_cast<ptrdiff_t>(off),
                       buf.begin() + static_cast<ptrdiff_t>(off) + pay_len);
    return true;
}

// ---------------------------------------------------------------------------
// JoinResponse  (server → client)
//
//   [kind:u8][world_seed:u64][config_len:u32][config_bytes...]
// ---------------------------------------------------------------------------

struct JoinResponsePayload {
    uint64_t             world_seed   = 0;
    std::vector<uint8_t> config_bytes;
};

inline std::vector<uint8_t> encode_join_response(const JoinResponsePayload& p)
{
    std::vector<uint8_t> buf;
    write_u8 (buf, static_cast<uint8_t>(NetPacketKind::JoinResponse));
    write_u64(buf, p.world_seed);
    write_u32(buf, static_cast<uint32_t>(p.config_bytes.size()));
    buf.insert(buf.end(), p.config_bytes.begin(), p.config_bytes.end());
    return buf;
}

inline bool decode_join_response(const std::vector<uint8_t>& buf, JoinResponsePayload& out)
{
    if (buf.empty() || buf[0] != static_cast<uint8_t>(NetPacketKind::JoinResponse))
        return false;
    size_t off = 1;
    if (off + 8 > buf.size()) return false;
    out.world_seed = read_u64(buf, off);
    if (off + 4 > buf.size()) return false;
    uint32_t len = read_u32(buf, off);
    if (off + len > buf.size()) return false;
    out.config_bytes.assign(buf.begin() + static_cast<ptrdiff_t>(off),
                             buf.begin() + static_cast<ptrdiff_t>(off) + len);
    return true;
}

// ---------------------------------------------------------------------------
// DirtyChunkData  (server → client)
//
//   [kind:u8][data_len:u32][chunk_file_bytes...]
// ---------------------------------------------------------------------------

struct DirtyChunkDataPayload {
    std::vector<uint8_t> chunk_bytes;
};

inline std::vector<uint8_t> encode_dirty_chunk_data(const DirtyChunkDataPayload& p)
{
    std::vector<uint8_t> buf;
    write_u8 (buf, static_cast<uint8_t>(NetPacketKind::DirtyChunkData));
    write_u32(buf, static_cast<uint32_t>(p.chunk_bytes.size()));
    buf.insert(buf.end(), p.chunk_bytes.begin(), p.chunk_bytes.end());
    return buf;
}

inline bool decode_dirty_chunk_data(const std::vector<uint8_t>& buf, DirtyChunkDataPayload& out)
{
    if (buf.empty() || buf[0] != static_cast<uint8_t>(NetPacketKind::DirtyChunkData))
        return false;
    size_t off = 1;
    if (off + 4 > buf.size()) return false;
    uint32_t len = read_u32(buf, off);
    if (off + len > buf.size()) return false;
    out.chunk_bytes.assign(buf.begin() + static_cast<ptrdiff_t>(off),
                            buf.begin() + static_cast<ptrdiff_t>(off) + len);
    return true;
}

} // namespace net
