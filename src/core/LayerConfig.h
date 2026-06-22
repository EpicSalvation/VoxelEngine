#pragma once

#include <optional>
#include <string>
#include <vector>

enum class VoxelMode {
    composite,  // lazy decomposition on demand; requires a decompose_to target and recipe plugin
    immutable,  // collision + rendering only; no decomposition, no persistence, no modification
    terminal,   // leaf layer; directly player-modifiable; always persisted when dirty
};

// Shape of a layer's camera-centered streaming residency volume (M16, L1).
// The volume replaces LODManager's old XZ-Chebyshev-disc × absolute-Y-band
// footprint with an axis-agnostic predicate so non-block-game worlds (deep
// descents, flying games, space) stream correctly:
//   box    — isotropic 3D Chebyshev cube, no vertical bias (the default; a box
//            volume reproduces the pre-M16 footprint byte-for-byte). Radius is
//            view_distance_chunks.
//   sphere — isotropic Euclidean ball of radius view_distance_chunks (excludes
//            the box corners).
//   shell  — a thin Euclidean band [view_distance - shell_thickness,
//            view_distance] for a backdrop that need only be resident at range.
enum class StreamingShape {
    box,
    sphere,
    shell,
};

struct LayerDef {
    std::string              name;
    double                   voxel_size_m  = 0.0;
    VoxelMode                mode          = VoxelMode::terminal;
    std::optional<std::string> decompose_to;  // composite layers only; names the child layer

    // Chunk streaming parameters (M3). Optional in the config; defaults applied when omitted.
    int chunk_size_voxels    = 32;  // voxels per chunk side; the chunk grid is chunk_size_voxels³
    int view_distance_chunks = 8;   // load/evict radius around the camera, measured in chunks

    // Per-layer resident-chunk cap (M10, ARCHITECTURE §11). 0 = unlimited.
    // When the manager's resident set for this layer exceeds the cap, the
    // farthest-first clean non-pending chunks are evicted to fit. Near chunks
    // and dirty (player-edited) chunks are always pinned regardless of the cap.
    int resident_chunk_budget = 0;

    // Per-layer decompose trigger distance in metres (the macro→child cascade
    // follow-up). A composite layer's macro voxels decompose into their child layer
    // once the camera is within this distance of the macro AABB surface. When unset,
    // the manager falls back to the single approachRadiusM passed to
    // DecompositionManager::tick(), so every config that omits it keeps the
    // pre-existing single-radius behaviour byte-for-byte. Setting it per layer
    // DECOUPLES the cascade steps: a deep stack can reveal its coarse child far out
    // (a large value on the coarse layer) yet only build its fine, expensive child
    // up close (a small value on the finer composite layer) — e.g. an asteroid's
    // 4 m silhouette at 280 m but its 1 m mineable grid only within 90 m.
    std::optional<double> decompose_distance_m;

    // Camera-centered streaming volume shape and radii (M16, L1). Optional in the
    // config; the default box reproduces the pre-M16 footprint, so existing
    // configs are byte-for-byte unchanged. The volume radius is view_distance_chunks;
    // shell_thickness_chunks names the band width for the shell shape only (inner
    // radius = view_distance_chunks − shell_thickness_chunks, clamped at 0).
    StreamingShape streaming_shape         = StreamingShape::box;
    int            shell_thickness_chunks  = 1;

    // Explicit interactive-layer selector (M16, L4). The single-layer World API
    // (getVoxel/setVoxel, dirty tracking, persistence, the collision substep
    // scale, picking) forwards to the layer flagged interactive: true. Optional;
    // when no layer is flagged, World falls back to the pre-M16 default (the first
    // terminal layer, then the first layer). At most one layer may be flagged —
    // LayerConfig hard-errors on two, so a mid-stack playspace is a first-class
    // declared choice rather than a silent first-in-order pick.
    bool           interactive            = false;
};

// Parses and validates a layer stack from a YAML project config file.
//
// All validation runs at construction time. A successfully constructed LayerConfig
// means the layer stack is coherent; downstream systems can trust it without re-checking.
// Invalid configs throw std::runtime_error with a descriptive message so the engine
// can exit at startup rather than failing silently at runtime.
//
// Validation performed:
//   - At least one layer defined
//   - voxel_size_m values are in strictly descending order (parent > child)
//   - Adjacent-layer size ratio is a whole integer >= 2
//   - Every composite layer has decompose_to naming a layer that exists in the config
class LayerConfig {
public:
    // Load from a YAML file. Throws std::runtime_error on parse or validation failure.
    static LayerConfig loadFromFile(const std::string& path);

    // Load from a YAML string (convenient for tests and inline configs).
    static LayerConfig loadFromString(const std::string& yaml);

    const std::vector<LayerDef>& layers() const { return layers_; }

    // Returns a pointer to the named layer, or nullptr if it does not exist.
    const LayerDef* findLayer(const std::string& name) const;

    // Returns the 0-based index of the named layer, or -1 if it does not exist.
    int layerIndex(const std::string& name) const;

private:
    LayerConfig() = default;
    static LayerConfig parseAndValidate(const std::string& yaml);

    std::vector<LayerDef> layers_;
};
