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
    // Recipe plugin registration is checked at engine startup after plugins are loaded.
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
