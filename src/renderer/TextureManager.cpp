#include "TextureManager.h"
#include "BgfxRenderer.h"
#include "MaterialFaces.h"

#include <bimg/decode.h>
#include <bx/allocator.h>
#include <bx/error.h>

#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

namespace texture {

TextureManager::TextureManager(PluginManager& pm, BgfxRenderer& renderer)
    : pm_(pm), renderer_(renderer) {
    pm_.setTextureManager(this);
}

TextureManager::~TextureManager() {
    pm_.setTextureManager(nullptr);  // no dangling pointer after this is gone
    // The atlas is engine-allocated GPU state; free it before bgfx::shutdown
    // (construct/destruct this between BgfxRenderer::initialize and shutdown).
    if (bgfx::isValid(atlas_)) bgfx::destroy(atlas_);
}

const AtlasTile* TextureManager::findTile(const std::string& texture_id) const {
    auto it = tiles_.find(texture_id);
    return it == tiles_.end() ? nullptr : &it->second;
}

void TextureManager::rebuild() {
    tiles_.clear();
    // Drop the prior atlas tile rects so material-face bindings (T4) re-resolve
    // against exactly the survivors of this rebuild — a torn-down texture's tile
    // disappears here, and its bindings fall back to the white tile.
    materialfaces::clearTiles();

    // Decode every registered image into an RGBA8 tile and stage it in the
    // headless packer. Ids are collected in the same order tiles are added, so
    // packed-tile index i maps back to ids[i].
    TextureAtlasData         atlasData;
    std::vector<std::string> ids;
    bx::DefaultAllocator     alloc;
    for (const RegisteredTexture& rt : pm_.textures()) {
        // Inline bytes (register_texture_data, e.g. a Blockbench embedded texture)
        // are decoded directly; otherwise read the encoded image from `path`.
        std::vector<uint8_t> bytes;
        if (!rt.data.empty()) {
            bytes = rt.data;
        } else {
            std::ifstream f(rt.path, std::ios::binary);
            if (!f) {
                std::cerr << "[TextureManager] cannot open texture '" << rt.texture_id
                          << "' at " << rt.path << "; skipped.\n";
                continue;
            }
            bytes.assign((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        }
        bx::Error err;
        bimg::ImageContainer* ic = bimg::imageParse(
            &alloc, bytes.data(), static_cast<uint32_t>(bytes.size()),
            bimg::TextureFormat::RGBA8, &err);
        if (!ic || !err.isOk()) {
            std::cerr << "[TextureManager] failed to decode texture '" << rt.texture_id
                      << "' at " << rt.path << "; skipped.\n";
            if (ic) bimg::imageFree(ic);
            continue;
        }
        const int idx = atlasData.addTile(static_cast<const uint8_t*>(ic->m_data),
                                          static_cast<uint16_t>(ic->m_width),
                                          static_cast<uint16_t>(ic->m_height));
        bimg::imageFree(ic);
        if (idx >= 0) ids.push_back(rt.texture_id);
    }

    // Pack first so tileCount() reflects the staged tiles: the packer fills its
    // tile table in pack(), so the "no surviving textures" check below must run
    // against the packed result, not the pre-pack (always-zero) count.
    atlasData.pack();

    // Replace the GPU atlas. Destroy first so a rebuild never leaks the prior one.
    if (bgfx::isValid(atlas_)) {
        bgfx::destroy(atlas_);
        atlas_ = BGFX_INVALID_HANDLE;
    }

    if (atlasData.tileCount() == 0) {
        // No surviving textures: revert the renderer to the 1×1 white tile so
        // colored worlds render unchanged (the teardown end state).
        renderer_.setAtlas(BGFX_INVALID_HANDLE);
        return;
    }

    const std::vector<uint8_t>& px = atlasData.pixels();
    // Point sampling + clamp: tiles are addressed by exact UV sub-rects, so
    // neither filtering across tile borders nor hardware REPEAT across the whole
    // atlas is wanted here (the in-tile repeat wrap is the shader's job in T5).
    atlas_ = bgfx::createTexture2D(
        atlasData.width(), atlasData.height(), false, 1,
        bgfx::TextureFormat::RGBA8,
        BGFX_SAMPLER_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
        bgfx::copy(px.data(), static_cast<uint32_t>(px.size())));

    for (size_t i = 0; i < ids.size(); ++i) {
        tiles_[ids[i]] = atlasData.tile(i);
        // Publish the resolved sub-rect so (palette_index, face) bindings (T4) the
        // mesh builder reads point at this atlas's tiles.
        materialfaces::setTileRect(ids[i], atlasData.tile(i));
    }

    renderer_.setAtlas(atlas_);
}

}  // namespace texture
