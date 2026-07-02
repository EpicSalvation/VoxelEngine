#pragma once

// Binary serialization of LayerConfig for the join handshake (M11).
//
// Binary format: [num_layers:u32] then for each layer:
//   [name_len:u32][name bytes]
//   [voxel_size_m:f64]
//   [mode:u8]  (0=composite, 1=immutable, 2=terminal)
//   [has_decompose_to:u8]
//   [decompose_to_len:u32][decompose_to bytes]  (only if has_decompose_to)
//   [chunk_size_voxels:u32]
//   [view_distance_chunks:u32]
//   [resident_chunk_budget:u32]
//
// Deserialization reconstructs a YAML string and calls LayerConfig::loadFromString
// so that all LayerConfig validation runs exactly once (private constructor rule).

#include "core/LayerConfig.h"
#include "net/NetPackets.h"

#include <cstdint>
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace net {

// Escape a network-supplied string for embedding in a YAML double-quoted
// scalar. Without this, a name containing `"` or a newline breaks out of the
// quoted scalar and injects arbitrary YAML into the reconstructed config.
inline std::string yamlEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c == '\\')      out += "\\\\";
        else if (c == '"')  out += "\\\"";
        else if (c < 0x20 || c == 0x7f) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\x%02X", c);
            out += buf;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

inline std::vector<uint8_t> serializeLayerConfig(const LayerConfig& config)
{
    const auto& layers = config.layers();
    std::vector<uint8_t> buf;
    write_u32(buf, static_cast<uint32_t>(layers.size()));
    for (const auto& ld : layers) {
        // name
        write_u32(buf, static_cast<uint32_t>(ld.name.size()));
        for (char c : ld.name) buf.push_back(static_cast<uint8_t>(c));

        // voxel_size_m
        write_f64(buf, ld.voxel_size_m);

        // mode: 0=composite, 1=immutable, 2=terminal
        uint8_t mode_byte = 0;
        switch (ld.mode) {
            case VoxelMode::composite: mode_byte = 0; break;
            case VoxelMode::immutable: mode_byte = 1; break;
            case VoxelMode::terminal:  mode_byte = 2; break;
        }
        write_u8(buf, mode_byte);

        // decompose_to
        if (ld.decompose_to.has_value()) {
            write_u8(buf, 1);
            write_u32(buf, static_cast<uint32_t>(ld.decompose_to->size()));
            for (char c : *ld.decompose_to) buf.push_back(static_cast<uint8_t>(c));
        } else {
            write_u8(buf, 0);
        }

        write_u32(buf, static_cast<uint32_t>(ld.chunk_size_voxels));
        write_u32(buf, static_cast<uint32_t>(ld.view_distance_chunks));
        write_u32(buf, static_cast<uint32_t>(ld.resident_chunk_budget));
    }
    return buf;
}

inline LayerConfig deserializeLayerConfig(const std::vector<uint8_t>& data)
{
    if (data.empty()) throw std::runtime_error("deserializeLayerConfig: empty data");

    size_t off = 0;
    if (off + 4 > data.size()) throw std::runtime_error("deserializeLayerConfig: truncated");
    uint32_t num_layers = read_u32(data, off);

    std::ostringstream yaml;
    yaml << "layers:\n";

    for (uint32_t i = 0; i < num_layers; ++i) {
        // name
        if (off + 4 > data.size()) throw std::runtime_error("deserializeLayerConfig: truncated name_len");
        uint32_t name_len = read_u32(data, off);
        if (off + name_len > data.size()) throw std::runtime_error("deserializeLayerConfig: truncated name");
        std::string name(reinterpret_cast<const char*>(data.data() + off), name_len);
        off += name_len;

        // voxel_size_m
        if (off + 8 > data.size()) throw std::runtime_error("deserializeLayerConfig: truncated voxel_size_m");
        double voxel_size_m = read_f64(data, off);

        // mode
        if (off + 1 > data.size()) throw std::runtime_error("deserializeLayerConfig: truncated mode");
        uint8_t mode_byte = read_u8(data, off);
        std::string mode_str;
        switch (mode_byte) {
            case 0: mode_str = "composite"; break;
            case 1: mode_str = "immutable"; break;
            case 2: mode_str = "terminal";  break;
            default: throw std::runtime_error("deserializeLayerConfig: unknown mode");
        }

        // decompose_to
        if (off + 1 > data.size()) throw std::runtime_error("deserializeLayerConfig: truncated has_decompose_to");
        uint8_t has_decompose_to = read_u8(data, off);
        std::string decompose_to;
        if (has_decompose_to) {
            if (off + 4 > data.size()) throw std::runtime_error("deserializeLayerConfig: truncated decompose_to_len");
            uint32_t dt_len = read_u32(data, off);
            if (off + dt_len > data.size()) throw std::runtime_error("deserializeLayerConfig: truncated decompose_to");
            decompose_to.assign(reinterpret_cast<const char*>(data.data() + off), dt_len);
            off += dt_len;
        }

        // chunk_size_voxels, view_distance_chunks, resident_chunk_budget
        if (off + 12 > data.size()) throw std::runtime_error("deserializeLayerConfig: truncated chunk params");
        uint32_t chunk_size_voxels    = read_u32(data, off);
        uint32_t view_distance_chunks = read_u32(data, off);
        uint32_t resident_chunk_budget = read_u32(data, off);

        yaml << "  - name: \"" << yamlEscape(name) << "\"\n";
        yaml << "    voxel_size_m: " << voxel_size_m << "\n";
        yaml << "    mode: " << mode_str << "\n";
        if (!decompose_to.empty()) {
            yaml << "    decompose_to: \"" << yamlEscape(decompose_to) << "\"\n";
        }
        yaml << "    chunk_size_voxels: " << chunk_size_voxels << "\n";
        yaml << "    view_distance_chunks: " << view_distance_chunks << "\n";
        yaml << "    resident_chunk_budget: " << resident_chunk_budget << "\n";
    }

    return LayerConfig::loadFromString(yaml.str());
}

} // namespace net
