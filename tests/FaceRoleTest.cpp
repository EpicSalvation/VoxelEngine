// Tests for M16 G1/G2 axis-agnostic face roles.
//
// A material's up / down / lateral tiles resolve to the geometric face opposing
// the supplied gravity vector through the shared axisrole::roleOf seam — grass
// renders on the +X face under a radial well, all six faces are independently
// bindable, and the SAME role resolution drives the RecipeDesc boundary
// distribution. Under the default constant -Y the resolved UVs and the boundary
// placement are byte-identical to the M15 top=+Y / bottom=-Y mapping (regression).

#include "world/AxisRole.h"
#include "world/ResolvedRecipe.h"
#include "world/Chunk.h"
#include "world/Voxel.h"
#include "renderer/MaterialFaces.h"
#include "renderer/ChunkMeshData.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>

using materialfaces::Face;
using texture::AtlasTile;

namespace {

AtlasTile rect(float u0, float v0, float u1, float v1) {
    AtlasTile t; t.u0 = u0; t.v0 = v0; t.u1 = u1; t.v1 = v1; t.w = 16; t.h = 16;
    return t;
}

}  // namespace

// ── axisrole::roleOf — the shared seam ──────────────────────────────────────

TEST(FaceRole, RoleOfResolvesAgainstGravity) {
    using axisrole::Role;
    using axisrole::roleOf;
    const glm::dvec3 negY{0, -1, 0};

    // Default -Y: +Y is up, -Y is down, the rest lateral.
    EXPECT_EQ(roleOf({0, 1, 0}, negY),  Role::Up);
    EXPECT_EQ(roleOf({0, -1, 0}, negY), Role::Down);
    EXPECT_EQ(roleOf({1, 0, 0}, negY),  Role::Lateral);
    EXPECT_EQ(roleOf({0, 0, 1}, negY),  Role::Lateral);

    // Gravity -X (radial well to the -X): +X is now "up".
    const glm::dvec3 negX{-1, 0, 0};
    EXPECT_EQ(roleOf({1, 0, 0}, negX),  Role::Up);
    EXPECT_EQ(roleOf({-1, 0, 0}, negX), Role::Down);
    EXPECT_EQ(roleOf({0, 1, 0}, negX),  Role::Lateral);

    // Zero-g: no privileged axis — every face is lateral.
    const glm::dvec3 zero{0, 0, 0};
    EXPECT_EQ(roleOf({0, 1, 0}, zero),  Role::Lateral);
    EXPECT_EQ(roleOf({1, 0, 0}, zero),  Role::Lateral);
}

// ── materialfaces::faceTile — gravity-aware tile resolution ──────────────────

TEST(FaceRole, DefaultGravityIsByteIdenticalToM15Mapping) {
    materialfaces::clearBindings();
    materialfaces::clearTiles();
    const AtlasTile top  = rect(0.00f, 0.00f, 0.25f, 0.25f);
    const AtlasTile bot  = rect(0.25f, 0.00f, 0.50f, 0.25f);
    const AtlasTile side = rect(0.50f, 0.00f, 0.75f, 0.25f);
    materialfaces::setTileRect("grass_top", top);
    materialfaces::setTileRect("dirt", bot);
    materialfaces::setTileRect("grass_side", side);
    materialfaces::setMaterialFaces(7, "grass_top", "dirt", "grass_side", 1.0f);

    // The 2-arg overload and an explicit -Y resolve to the historical mapping:
    // +Y → top, -Y → bottom, the four laterals → side.
    struct E { int face; const AtlasTile* tile; };
    const E expects[] = {
        {Face::PosY, &top}, {Face::NegY, &bot},
        {Face::PosZ, &side}, {Face::NegZ, &side},
        {Face::NegX, &side}, {Face::PosX, &side},
    };
    for (const E& e : expects) {
        const auto a = materialfaces::faceTile(7, e.face);
        const auto b = materialfaces::faceTile(7, e.face, glm::dvec3(0, -1, 0));
        ASSERT_TRUE(a.bound);
        EXPECT_FLOAT_EQ(a.tile.u0, e.tile->u0) << "face " << e.face;
        EXPECT_FLOAT_EQ(b.tile.u0, e.tile->u0) << "face " << e.face;
        EXPECT_FLOAT_EQ(a.tile.u1, e.tile->u1) << "face " << e.face;
    }
}

TEST(FaceRole, GrassRendersOnPlusXFaceUnderRadialGravity) {
    materialfaces::clearBindings();
    materialfaces::clearTiles();
    const AtlasTile top  = rect(0.0f, 0.0f, 0.25f, 0.25f);
    const AtlasTile bot  = rect(0.25f, 0.0f, 0.50f, 0.25f);
    const AtlasTile side = rect(0.50f, 0.0f, 0.75f, 0.25f);
    materialfaces::setTileRect("grass_top", top);
    materialfaces::setTileRect("dirt", bot);
    materialfaces::setTileRect("grass_side", side);
    materialfaces::setMaterialFaces(7, "grass_top", "dirt", "grass_side", 1.0f);

    // "Down" points -X (toward a body center to the -X): +X is "up", so the +X
    // face shows the grass top; -X shows dirt; every other face is side.
    const glm::dvec3 down{-1, 0, 0};
    EXPECT_FLOAT_EQ(materialfaces::faceTile(7, Face::PosX, down).tile.u0, top.u0);
    EXPECT_FLOAT_EQ(materialfaces::faceTile(7, Face::NegX, down).tile.u0, bot.u0);
    EXPECT_FLOAT_EQ(materialfaces::faceTile(7, Face::PosY, down).tile.u0, side.u0);
    EXPECT_FLOAT_EQ(materialfaces::faceTile(7, Face::NegZ, down).tile.u0, side.u0);
}

TEST(FaceRole, AllSixFacesIndependentlyBindable) {
    materialfaces::clearBindings();
    materialfaces::clearTiles();
    const AtlasTile t[6] = {
        rect(0.0f, 0.0f, 0.1f, 0.1f), rect(0.1f, 0.0f, 0.2f, 0.1f),
        rect(0.2f, 0.0f, 0.3f, 0.1f), rect(0.3f, 0.0f, 0.4f, 0.1f),
        rect(0.4f, 0.0f, 0.5f, 0.1f), rect(0.5f, 0.0f, 0.6f, 0.1f),
    };
    const char* ids[6] = {"pz", "nz", "ny", "py", "nx", "px"};
    for (int i = 0; i < 6; ++i) materialfaces::setTileRect(ids[i], t[i]);
    materialfaces::setMaterialFacesAll(9, ids[0], ids[1], ids[2], ids[3], ids[4], ids[5],
                                       1.0f);

    // Under the default -Y frame each geometric face shows its own distinct tile.
    for (int f = 0; f < materialfaces::kFaceCount; ++f) {
        const auto ft = materialfaces::faceTile(9, f);
        ASSERT_TRUE(ft.bound) << "face " << f;
        EXPECT_FLOAT_EQ(ft.tile.u0, t[f].u0) << "face " << f;
    }
}

TEST(FaceRole, MeshBuilderResolvesFacesAgainstSuppliedGravity) {
    materialfaces::clearBindings();
    materialfaces::clearTiles();
    // Distinctive sub-rect (u0=0.5) so a bound grass face is told apart from the
    // unbound white rect (u0=0, u1=1).
    const AtlasTile top = rect(0.5f, 0.0f, 0.75f, 0.25f);
    materialfaces::setTileRect("grass_top", top);
    materialfaces::setMaterialFaces(7, "grass_top", nullptr, nullptr, 1.0f);

    Chunk chunk(ChunkCoord{0, 0, 0}, 4, WorldCoord());
    Voxel v; v.material.palette_index = 7; v.material.density = 1.0f;
    chunk.at(1, 1, 1) = v;

    auto faceU0 = [](const std::vector<MeshVertex>& verts, int faceIdx) {
        return verts[static_cast<size_t>(faceIdx) * 6].r0;  // u0 of the face's tile
    };

    // Default -Y: the +Y face carries the grass tile (u0=0.5); +X is unbound
    // (white, u0=0).
    std::vector<MeshVertex> defVerts; std::vector<uint32_t> a, b;
    buildChunkMeshData(chunk, defVerts, a, b, 1.0);
    EXPECT_FLOAT_EQ(faceU0(defVerts, Face::PosY), 0.5f);
    EXPECT_FLOAT_EQ(faceU0(defVerts, Face::PosX), 0.0f);

    // Radial-down -X: now the +X face carries the grass tile and +Y is white.
    std::vector<MeshVertex> radVerts; std::vector<uint32_t> c, d;
    buildChunkMeshData(chunk, radVerts, c, d, 1.0, glm::dvec3(-1, 0, 0));
    EXPECT_FLOAT_EQ(faceU0(radVerts, Face::PosX), 0.5f);
    EXPECT_FLOAT_EQ(faceU0(radVerts, Face::PosY), 0.0f);
}

// ── RecipeDesc boundary distribution — same role seam (G2) ───────────────────

namespace {

ResolvedDistribution oneMaterial(uint8_t palette) {
    ResolvedDistribution d;
    MaterialProperties props{};
    props.palette_index = palette;
    props.density = 1.0f;
    d.materials.push_back(ResolvedWeight{props, 1.0f});
    d.noise = nullptr;  // null ⇒ first material everywhere (deterministic)
    return d;
}

// Build a recipe whose top/bottom/interior paint distinct palette indices, so the
// row a face role lands on is identifiable.
ResolvedRecipe rolesRecipe() {
    ResolvedRecipe r;
    r.interior = oneMaterial(50);
    r.top.distribution = oneMaterial(60);   r.top.depth = 1;    r.top.present = true;
    r.bottom.distribution = oneMaterial(70); r.bottom.depth = 1; r.bottom.present = true;
    return r;
}

uint8_t paletteAt(const Chunk& c, int x, int y, int z) {
    return c.at(x, y, z).material.palette_index;
}

}  // namespace

TEST(FaceRole, RecipeBoundaryRolesFollowGravity) {
    const ResolvedRecipe recipe = rolesRecipe();
    const int n = 8;            // one macro voxel's 8³ child grid fills the chunk
    const int64_t ratio = 8;

    auto fill = [&](const glm::dvec3& gravity) {
        Chunk chunk(ChunkCoord{0, 0, 0}, n, WorldCoord());
        fillChildChunk(chunk, 1.0, recipe, chunkmath::VoxelCoord{0, 0, 0}, ratio,
                       /*seed*/ 12345, gravity);
        return chunk;
    };

    // Default -Y: bottom (palette 70) lands on the low-Y face, top (60) on high-Y,
    // interior (50) between.
    Chunk def = fill(glm::dvec3(0, -1, 0));
    EXPECT_EQ(paletteAt(def, 2, 0, 2), 70);  // y=0 → bottom
    EXPECT_EQ(paletteAt(def, 2, 7, 2), 60);  // y=7 → top
    EXPECT_EQ(paletteAt(def, 2, 4, 2), 50);  // middle → interior

    // The default (gravity arg omitted) must match the explicit -Y exactly.
    Chunk implicitDef(ChunkCoord{0, 0, 0}, n, WorldCoord());
    fillChildChunk(implicitDef, 1.0, recipe, chunkmath::VoxelCoord{0, 0, 0}, ratio, 12345);
    for (int y = 0; y < n; ++y)
        EXPECT_EQ(paletteAt(def, 2, y, 2), paletteAt(implicitDef, 2, y, 2)) << "y=" << y;

    // Flip gravity to +Y: the roles flip — bottom now lands on high-Y, top on low-Y.
    Chunk up = fill(glm::dvec3(0, 1, 0));
    EXPECT_EQ(paletteAt(up, 2, 0, 2), 60);  // y=0 → top now
    EXPECT_EQ(paletteAt(up, 2, 7, 2), 70);  // y=7 → bottom now
}
