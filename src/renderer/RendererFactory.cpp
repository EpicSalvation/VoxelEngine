#include "renderer/RendererFactory.h"

#include "renderer/BgfxRenderer.h"

// The factory body is the ONLY public-API-reachable code that names the
// concrete bgfx-backed renderer. BgfxRenderer.h pulls in <bgfx/bgfx.h>, so this
// translation unit lives under src/ (PRIVATE) and bgfx never crosses the
// include/ boundary — out-of-tree consumers see only the abstract Renderer
// returned here (docs/ARCHITECTURE.md §12).
std::unique_ptr<Renderer> createRenderer() {
    return std::make_unique<BgfxRenderer>();
}
