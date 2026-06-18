#pragma once

// TextureManager — owns the GPU texture atlas the voxel shader samples (M15 T3).
// It is the texture analog of AudioManager: where AudioManager owns the audio
// backend and loads register_sound assets, TextureManager owns the bgfx atlas
// texture and loads register_texture assets. It decodes each registered image
// (via bimg), packs them into one atlas (TextureAtlasData), uploads it through
// bgfx, and binds it to the renderer.
//
// Ownership / teardown (ARCHITECTURE §8, the M12 audio model):
//   register_texture records (texture_id, path, owner) in PluginManager. On
//   plugin unload PluginManager prunes that owner's entries, then calls
//   rebuild() here — so a plugin's tiles vanish from the atlas with it. The
//   atlas GPU handle is owned solely by this class; the renderer only references
//   it (BgfxRenderer::setAtlas takes no ownership).
//
// Dependency rules:
//   Depends on: PluginManager (texture registry), BgfxRenderer (atlas binding),
//               bgfx + bimg (GPU upload + image decode)
//   Must NOT depend on: World, PhysicsSystem, simulation/IO tiers

#include "renderer/TextureAtlasData.h"
#include "core/PluginManager.h"   // PluginId, RegisteredTexture
#include <bgfx/bgfx.h>
#include <string>
#include <unordered_map>

class BgfxRenderer;

namespace texture {

class TextureManager {
public:
    // Self-registers with PluginManager (setTextureManager(this)) so register_texture
    // records and plugin-unload teardown route here. bgfx must already be
    // initialized (construct after BgfxRenderer::initialize). Call rebuild() once
    // after the initial plugin load to ingest textures registered before this
    // existed (the preloadSounds() analog).
    TextureManager(PluginManager& pm, BgfxRenderer& renderer);
    ~TextureManager();

    // Decode every texture in the PluginManager registry, pack them into one
    // atlas, upload it, and bind it to the renderer. Reads the live registry, so
    // after a plugin loads or unloads this reflects exactly the survivors — the
    // owner-tracked teardown. With no textures registered the atlas is destroyed
    // and the renderer reverts to its built-in 1×1 white tile.
    void rebuild();

    // Atlas handle (BGFX_INVALID_HANDLE when no textures are registered).
    bgfx::TextureHandle atlas() const { return atlas_; }

    // Tile lookup by texture_id; nullptr when unknown. Consumed by the
    // (palette_index, face) → tile binding (T4) and the Blockbench importer (T6).
    const AtlasTile* findTile(const std::string& texture_id) const;
    size_t           tileCount() const { return tiles_.size(); }

private:
    PluginManager&      pm_;
    BgfxRenderer&       renderer_;
    bgfx::TextureHandle atlas_ = BGFX_INVALID_HANDLE;
    std::unordered_map<std::string, AtlasTile> tiles_;  // texture_id → UV rect
};

}  // namespace texture
