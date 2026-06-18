#include "LayerConfig.h"
#include <yaml-cpp/yaml.h>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>

static VoxelMode parseModeString(const std::string& s, const std::string& layerName) {
    if (s == "composite") return VoxelMode::composite;
    if (s == "immutable") return VoxelMode::immutable;
    if (s == "terminal")  return VoxelMode::terminal;
    throw std::runtime_error(
        "Layer '" + layerName + "': unknown mode '" + s +
        "'. Expected 'composite', 'immutable', or 'terminal'.");
}

static StreamingShape parseShapeString(const std::string& s, const std::string& layerName) {
    if (s == "box")    return StreamingShape::box;
    if (s == "sphere") return StreamingShape::sphere;
    if (s == "shell")  return StreamingShape::shell;
    throw std::runtime_error(
        "Layer '" + layerName + "': unknown streaming_volume shape '" + s +
        "'. Expected 'box', 'sphere', or 'shell'.");
}

LayerConfig LayerConfig::loadFromFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open())
        throw std::runtime_error("Cannot open layer config file: " + path);
    std::ostringstream buf;
    buf << file.rdbuf();
    return parseAndValidate(buf.str());
}

LayerConfig LayerConfig::loadFromString(const std::string& yaml) {
    return parseAndValidate(yaml);
}

LayerConfig LayerConfig::parseAndValidate(const std::string& yamlContent) {
    YAML::Node root;
    try {
        root = YAML::Load(yamlContent);
    } catch (const YAML::Exception& e) {
        throw std::runtime_error(std::string("YAML parse error: ") + e.what());
    }

    if (!root["layers"] || !root["layers"].IsSequence())
        throw std::runtime_error("Layer config must contain a 'layers' sequence.");

    LayerConfig config;

    for (const auto& node : root["layers"]) {
        LayerDef def;

        if (!node["name"])
            throw std::runtime_error("Each layer entry must have a 'name' field.");
        def.name = node["name"].as<std::string>();

        if (!node["voxel_size_m"])
            throw std::runtime_error("Layer '" + def.name + "' missing 'voxel_size_m'.");
        def.voxel_size_m = node["voxel_size_m"].as<double>();
        if (def.voxel_size_m <= 0.0)
            throw std::runtime_error("Layer '" + def.name + "': voxel_size_m must be > 0.");

        if (!node["mode"])
            throw std::runtime_error("Layer '" + def.name + "' missing 'mode'.");
        def.mode = parseModeString(node["mode"].as<std::string>(), def.name);

        if (node["decompose_to"])
            def.decompose_to = node["decompose_to"].as<std::string>();

        // Chunk streaming parameters are optional; defaults (set in LayerDef) apply when absent.
        if (node["chunk_size_voxels"]) {
            def.chunk_size_voxels = node["chunk_size_voxels"].as<int>();
            if (def.chunk_size_voxels < 1)
                throw std::runtime_error(
                    "Layer '" + def.name + "': chunk_size_voxels must be >= 1 (got " +
                    std::to_string(def.chunk_size_voxels) + ").");
        }
        if (node["view_distance_chunks"]) {
            def.view_distance_chunks = node["view_distance_chunks"].as<int>();
            if (def.view_distance_chunks < 0)
                throw std::runtime_error(
                    "Layer '" + def.name + "': view_distance_chunks must be >= 0 (got " +
                    std::to_string(def.view_distance_chunks) + ").");
        }
        if (node["resident_chunk_budget"]) {
            def.resident_chunk_budget = node["resident_chunk_budget"].as<int>();
            if (def.resident_chunk_budget < 0)
                throw std::runtime_error(
                    "Layer '" + def.name + "': resident_chunk_budget must be >= 0 (got " +
                    std::to_string(def.resident_chunk_budget) + "). Use 0 for unlimited.");
        }

        // Streaming volume (M16, L1). Optional nested map; the default box keeps
        // existing configs byte-for-byte unchanged. The volume radius is taken
        // from view_distance_chunks (above); only the shape and the shell band
        // thickness live here.
        if (node["streaming_volume"]) {
            const YAML::Node& vol = node["streaming_volume"];
            if (!vol.IsMap())
                throw std::runtime_error(
                    "Layer '" + def.name + "': streaming_volume must be a map with a "
                    "'shape' field (box | sphere | shell).");
            if (vol["shape"])
                def.streaming_shape = parseShapeString(vol["shape"].as<std::string>(), def.name);
            if (vol["shell_thickness_chunks"]) {
                def.shell_thickness_chunks = vol["shell_thickness_chunks"].as<int>();
                if (def.shell_thickness_chunks < 1)
                    throw std::runtime_error(
                        "Layer '" + def.name + "': shell_thickness_chunks must be >= 1 (got " +
                        std::to_string(def.shell_thickness_chunks) + ").");
            }
        }

        config.layers_.push_back(def);
    }

    if (config.layers_.empty())
        throw std::runtime_error("At least one layer must be defined.");

    // Sizes must be in strictly descending order; ratios must be whole integers >= 2.
    for (size_t i = 1; i < config.layers_.size(); ++i) {
        const auto& parent = config.layers_[i - 1];
        const auto& child  = config.layers_[i];

        if (child.voxel_size_m >= parent.voxel_size_m)
            throw std::runtime_error(
                "Layer '" + child.name + "' voxel_size_m (" +
                std::to_string(child.voxel_size_m) +
                ") must be smaller than parent layer '" + parent.name + "' (" +
                std::to_string(parent.voxel_size_m) + ").");

        double ratio   = parent.voxel_size_m / child.voxel_size_m;
        double rounded = std::round(ratio);
        if (std::abs(ratio - rounded) > 1e-9)
            throw std::runtime_error(
                "Layer ratio between '" + parent.name + "' and '" + child.name +
                "' must be a whole integer (got " + std::to_string(ratio) + "). "
                "Non-integer ratios cannot tile the child voxel grid cleanly.");
        if (rounded < 2.0)
            throw std::runtime_error(
                "Minimum ratio between adjacent layers is 2:1. Got " +
                std::to_string(static_cast<int>(rounded)) + ":1 between '" +
                parent.name + "' and '" + child.name + "'.");
    }

    // Every composite layer must name a decompose_to target that exists in this config.
    // Recipe references (feature/noise ids) are checked after plugins are loaded —
    // see validateRecipes() in core/RecipeValidation.h.
    for (const auto& layer : config.layers_) {
        if (layer.mode == VoxelMode::composite) {
            if (!layer.decompose_to)
                throw std::runtime_error(
                    "Composite layer '" + layer.name + "' must specify 'decompose_to'. "
                    "A composite layer with no decompose target has no valid runtime behavior.");
            if (!config.findLayer(*layer.decompose_to))
                throw std::runtime_error(
                    "Composite layer '" + layer.name +
                    "' references unknown decompose_to target '" + *layer.decompose_to + "'.");
        }
    }

    // Coarse-supersets-fine invariant (M10, ARCHITECTURE §4):
    // For every composite → child composite transition, the parent voxel size should
    // be at least as large as the child layer's chunk world size. If it is smaller,
    // one parent macro voxel decomposes into a child chunk that is *larger* than
    // the parent voxel's subvolume; the extra child voxels outside that subvolume
    // are left empty (fillChildChunk enforces this), but it means the child chunks
    // from adjacent parent voxels overlap in chunk-coord space — the last write
    // wins. This is logged as a warning so the config author can widen parent voxels
    // or reduce child chunk sizes to avoid the overlap. The check is informational
    // for composite→terminal transitions where the overlap is benign (terminal
    // chunks are not further decomposed).
    for (const auto& layer : config.layers_) {
        if (layer.mode != VoxelMode::composite) continue;
        const LayerDef* child = config.findLayer(*layer.decompose_to);
        if (!child) continue;  // already caught above
        const double childChunkWorldSize =
            child->voxel_size_m * static_cast<double>(child->chunk_size_voxels);
        if (child->mode == VoxelMode::composite &&
                layer.voxel_size_m < childChunkWorldSize - 1e-9) {
            throw std::runtime_error(
                "Coarse-supersets-fine violation (ARCHITECTURE §4): composite layer '" +
                layer.name + "' (voxel_size_m=" + std::to_string(layer.voxel_size_m) +
                ") decomposes into composite layer '" + child->name +
                "' whose chunk world size is " +
                std::to_string(childChunkWorldSize) +
                " m (child voxel_size_m=" + std::to_string(child->voxel_size_m) +
                " × chunk_size_voxels=" + std::to_string(child->chunk_size_voxels) +
                "). Parent voxel must be >= child chunk world size so each parent "
                "macro voxel fills exactly one or more complete child chunks. "
                "Reduce chunk_size_voxels for layer '" + child->name +
                "' or increase voxel_size_m for layer '" + layer.name + "'.");
        }
    }

    return config;
}

const LayerDef* LayerConfig::findLayer(const std::string& name) const {
    for (const auto& layer : layers_)
        if (layer.name == name)
            return &layer;
    return nullptr;
}

int LayerConfig::layerIndex(const std::string& name) const {
    for (int i = 0; i < static_cast<int>(layers_.size()); ++i)
        if (layers_[i].name == name)
            return i;
    return -1;
}
