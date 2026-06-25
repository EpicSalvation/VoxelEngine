#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct LayerChunkMetrics {
    std::string layerName;
    size_t      residentChunks   = 0;
    size_t      decomposedMacros = 0;
};

struct EngineMetrics {
    double  frameTimeSec  = 0.0;
    size_t  drawCalls     = 0;
    size_t  voiceCount    = 0;
    size_t  decompInFlight = 0;
    std::vector<LayerChunkMetrics> layers;

    size_t totalResidentChunks() const {
        size_t n = 0;
        for (const auto& l : layers) n += l.residentChunks;
        return n;
    }
};
