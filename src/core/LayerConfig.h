#pragma once

#include <optional>
#include <string>
#include <vector>

enum class VoxelMode {
    composite,  // lazy decomposition on demand; requires a decompose_to target and recipe plugin
    immutable,  // collision + rendering only; no decomposition, no persistence, no modification
    terminal,   // leaf layer; directly player-modifiable; always persisted when dirty
};

struct LayerDef {
    std::string              name;
    double                   voxel_size_m  = 0.0;
    VoxelMode                mode          = VoxelMode::terminal;
    std::optional<std::string> decompose_to;  // composite layers only; names the child layer
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
