#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "world/Chunk.h"

class World;

// Internal save format for player-edited (dirty) terminal-layer chunks (M5).
//
// This is the engine's OWN persistence format — deliberately distinct from the
// M7 .vox/.vxe interop formats (which exist for editor round-tripping, not save
// games). The on-disk layout per chunk is a small header carrying the world
// identity and the chunk coord, then a palette + run-length encoding of the
// voxel grid (most chunks contain a handful of distinct materials in long runs,
// so RLE over a deduplicated material palette is compact).
//
// Only dirty chunks are persisted; clean generator-produced chunks regenerate
// deterministically on cache miss and are never written. See ARCHITECTURE §11.
namespace persistence {

// Identifies the world a save belongs to, so a chunk file can be matched back to
// the layer config that produced it. A file whose identity differs from the live
// world is rejected on load rather than silently misinterpreted.
struct WorldIdentity {
    double voxel_size_m      = 0.0;
    int    chunk_size_voxels = 0;

    bool operator==(const WorldIdentity& rhs) const {
        return voxel_size_m == rhs.voxel_size_m &&
               chunk_size_voxels == rhs.chunk_size_voxels;
    }
};

// Encode a full chunk file (header + palette + RLE) for `chunk` under `id`.
std::vector<uint8_t> encodeChunkFile(const Chunk& chunk, const WorldIdentity& id);

// Decode a chunk file produced by encodeChunkFile. Returns nullptr if the bytes
// are malformed/truncated, the magic or version mismatch, or the stored identity
// differs from `id` (a stale or foreign file). The chunk's coord, size, and
// world-space origin are reconstructed from the header + identity. The returned
// chunk is clean (its edits are already on disk).
std::unique_ptr<Chunk> decodeChunkFile(const uint8_t* data, size_t size,
                                       const WorldIdentity& id);

// Like decodeChunkFile but skips the identity check. Used by the network
// handshake path where the client accepts any identity embedded in the file.
std::unique_ptr<Chunk> decodeChunkFilePermissive(const uint8_t* data, size_t size);

// Manages a directory of saved chunks for one world. One file per ChunkCoord.
class WorldSave {
public:
    // dir is created if it does not exist. id is stamped into every chunk file
    // and validated on load.
    WorldSave(std::string dir, WorldIdentity id);

    bool hasChunk(ChunkCoord coord) const;

    // Encode and write a single chunk to disk. Returns false on I/O failure.
    bool saveChunk(const Chunk& chunk) const;

    // Save every dirty chunk of `world`, clearing each chunk's dirty flag as it
    // is written. Returns the number of chunks saved.
    int saveDirtyChunks(World& world) const;

    // Load the saved chunk at coord, or nullptr if no valid file exists.
    std::unique_ptr<Chunk> tryLoadChunk(ChunkCoord coord) const;

    const std::string& directory() const { return dir_; }

    // List all chunk coordinates that have a saved file in this directory.
    std::vector<ChunkCoord> listSavedChunks() const;

    // Read raw file bytes for a saved chunk without decoding. Returns empty if not found.
    std::vector<uint8_t> loadRawChunkBytes(ChunkCoord coord) const;

    // Expose the world identity this save was created with.
    const WorldIdentity& identity() const { return id_; }

private:
    std::string filePath(ChunkCoord coord) const;

    std::string   dir_;
    WorldIdentity id_;
};

}  // namespace persistence
