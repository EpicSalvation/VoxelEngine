#pragma once

#include <memory>

#include "renderer/Renderer.h"

// Renderer creation factory — the public seam between an out-of-tree game and
// the engine's concrete graphics backend (docs/ARCHITECTURE.md §12).
//
// The engine's only shipped backend is bgfx-based, but bgfx is a PRIVATE
// dependency that must never appear in a public header: leaking bgfx handles
// across the library boundary would drag the graphics ABI into every consumer
// and make a future shared build far harder. So a third-party game never names
// the concrete renderer type — it asks the factory for an abstract `Renderer`
// and drives it through that interface alone. The concrete type and its bgfx
// includes stay entirely inside src/ (RendererFactory.cpp), reachable only by
// the privileged in-tree demos that opt into src/ directly.
//
// The returned renderer is uninitialized; call `initialize()` with the host's
// native window handles before use, and `shutdown()` before destruction.
std::unique_ptr<Renderer> createRenderer();
