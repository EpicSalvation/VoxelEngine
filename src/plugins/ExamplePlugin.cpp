#include "ExamplePlugin.h"
#include "../world/Voxel.h"
#include <iostream>

// Example layer generator: flat stone ground with a grass surface layer.
// chunk_origin is the world-space position of the chunk corner.
// The grid is grid_size³, laid out row-major with x varying fastest.
static void example_layer_generator(
    WorldCoord /*chunk_origin*/, int grid_size, Voxel* out_voxels, void* /*user_data*/)
{
    MaterialProperties stone;
    stone.density             = 2700.0f;
    stone.structural_strength = 0.9f;
    stone.thermal_conductivity = 2.0f;
    stone.hardness            = 0.7f;
    stone.palette_index       = 1;

    MaterialProperties grass;
    grass.density             = 1200.0f;
    grass.structural_strength = 0.3f;
    grass.thermal_conductivity = 0.5f;
    grass.hardness            = 0.2f;
    grass.palette_index       = 2;

    int surface_y = grid_size / 2;

    for (int z = 0; z < grid_size; ++z) {
        for (int y = 0; y < grid_size; ++y) {
            for (int x = 0; x < grid_size; ++x) {
                Voxel& v = out_voxels[x + grid_size * (y + grid_size * z)];
                if (y < surface_y - 1)
                    v.material = stone;
                else if (y == surface_y - 1)
                    v.material = grass;
                else
                    v = Voxel::empty();
            }
        }
    }
}

extern "C" int voxel_plugin_init(PluginContext* ctx) {
    MaterialProperties stone;
    stone.density             = 2700.0f;
    stone.structural_strength = 0.9f;
    stone.thermal_conductivity = 2.0f;
    stone.hardness            = 0.7f;
    stone.palette_index       = 1;
    ctx->register_material(ctx, "stone", stone);

    MaterialProperties grass;
    grass.density             = 1200.0f;
    grass.structural_strength = 0.3f;
    grass.thermal_conductivity = 0.5f;
    grass.hardness            = 0.2f;
    grass.palette_index       = 2;
    ctx->register_material(ctx, "grass", grass);

    ctx->register_layer_generator(ctx, "terrain", example_layer_generator, nullptr);

    std::cout << "[ExamplePlugin] Registered materials: stone, grass. "
              << "Layer generator: terrain.\n";
    return 0;
}
