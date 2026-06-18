#include "TextureAtlasData.h"

#include <algorithm>

namespace texture {

namespace {
// Smallest power of two >= v (with a floor of 1). Atlas dimensions are kept
// power-of-two so they are valid on every backend's sampler without NPOT caveats.
uint16_t nextPow2(uint32_t v) {
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return static_cast<uint16_t>(p);
}
}  // namespace

int TextureAtlasData::addTile(const uint8_t* rgba, uint16_t w, uint16_t h) {
    if (!rgba || w == 0 || h == 0) return -1;
    Source s;
    s.w = w;
    s.h = h;
    s.rgba.assign(rgba, rgba + static_cast<size_t>(w) * h * 4);
    sources_.push_back(std::move(s));
    return static_cast<int>(sources_.size() - 1);
}

void TextureAtlasData::pack(uint16_t targetWidth) {
    tiles_.clear();
    pixels_.clear();
    width_ = height_ = 0;
    if (sources_.empty()) return;

    // Atlas width: the next power of two that holds both the requested target
    // width and the widest tile (a tile can never be wider than the atlas).
    uint16_t widest = 1;
    for (const Source& s : sources_)
        if (s.w > widest) widest = s.w;
    width_ = nextPow2(std::max<uint32_t>(targetWidth, widest));

    // Shelf-place every tile left-to-right, wrapping to a new row when the
    // current shelf fills. Record each tile's pixel rect; UVs are filled once
    // the final height is known.
    struct Placement { uint16_t x, y, w, h; };
    std::vector<Placement> places;
    places.reserve(sources_.size());
    uint16_t cursorX = 0, shelfY = 0, shelfH = 0;
    for (const Source& s : sources_) {
        if (cursorX + s.w > width_) {     // wrap to the next shelf
            shelfY = static_cast<uint16_t>(shelfY + shelfH);
            cursorX = 0;
            shelfH = 0;
        }
        places.push_back({cursorX, shelfY, s.w, s.h});
        cursorX = static_cast<uint16_t>(cursorX + s.w);
        if (s.h > shelfH) shelfH = s.h;
    }
    height_ = nextPow2(static_cast<uint32_t>(shelfY) + shelfH);

    // Blit each tile into the atlas buffer (zero-initialized: gaps are
    // transparent black) and compute its normalized UV rect.
    pixels_.assign(static_cast<size_t>(width_) * height_ * 4, 0);
    tiles_.resize(sources_.size());
    for (size_t i = 0; i < sources_.size(); ++i) {
        const Source&    s = sources_[i];
        const Placement& p = places[i];
        for (uint16_t row = 0; row < s.h; ++row) {
            const uint8_t* src = s.rgba.data() + static_cast<size_t>(row) * s.w * 4;
            uint8_t*       dst = pixels_.data() +
                                 (static_cast<size_t>(p.y + row) * width_ + p.x) * 4;
            std::copy(src, src + static_cast<size_t>(s.w) * 4, dst);
        }
        AtlasTile& t = tiles_[i];
        t.w  = s.w;
        t.h  = s.h;
        t.u0 = static_cast<float>(p.x)          / static_cast<float>(width_);
        t.v0 = static_cast<float>(p.y)          / static_cast<float>(height_);
        t.u1 = static_cast<float>(p.x + s.w)    / static_cast<float>(width_);
        t.v1 = static_cast<float>(p.y + s.h)    / static_cast<float>(height_);
    }
}

}  // namespace texture
