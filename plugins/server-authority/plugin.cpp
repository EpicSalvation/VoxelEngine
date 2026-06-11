// server-authority plugin — the built-in authoritative-server authority model.
//
// Authority model as plugin policy (M11, ARCHITECTURE §15):
//
//   Server / HostPeer role
//   ──────────────────────
//   The NetworkManager already routes all voxel writes through its single
//   applyEdit() choke point, which calls registered authority policies and the
//   on_edit_received hook before committing.  This plugin registers an explicit
//   on_edit_received handler that returns Apply (last-write-wins), making the
//   server-authority contract named, loadable, and replaceable without any engine
//   change.  A developer who wants a different authority model (e.g. area
//   ownership, griefing protection, optimistic P2P) loads a different plugin that
//   registers its own on_edit_received — this plugin does not need to be present.
//
//   Client role
//   ───────────
//   On a Client node, NetworkManager::applyEdit() forwards edit intents to the
//   server over the wire instead of applying them locally; the server commits via
//   this plugin's handler and broadcasts the committed edit back.  No client-side
//   registration is needed here — the routing is in NetworkManager.
//
//   HostPeer role
//   ─────────────
//   NetworkManager::startHostPeer() sets role == HostPeer, which is treated
//   identically to Server in the authority code path.  The same plugin is loaded
//   in both the dedicated-server and the host-as-authority P2P configuration; no
//   separate plugin binary is required.

#include "plugin_api.h"

#if defined(_WIN32)
#  define VOXEL_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define VOXEL_PLUGIN_EXPORT extern "C"
#endif

namespace {

// on_edit_received handler: last-write-wins authority policy.
// Returns Apply unconditionally — every edit that reaches the authority is
// accepted.  Replace this logic (or register a different plugin) for custom
// authority behaviour (e.g. whitelist by player id, reject out-of-bounds writes).
EditResolution on_edit_received(
    PlayerId      /*proposing_player*/,
    WorldCoord    /*position*/,
    const Voxel*  /*proposed_voxel*/,
    Voxel*        /*out_voxel*/,
    void*         /*user_data*/)
{
    return EditResolution::Apply;
}

// authority_policy validator: forward all intents unconditionally.
// This is already the default when no policy is registered, but providing it
// explicitly makes the choice auditable and lets a game override only this
// function to add coarse per-peer filtering (e.g. rate limiting) without
// replacing the full on_edit_received handler.
bool authority_policy(
    PlayerId     /*peer_id*/,
    WorldCoord   /*position*/,
    const Voxel* /*proposed_voxel*/,
    void*        /*user_data*/)
{
    return true;  // forward all edit intents to on_edit_received
}

} // anonymous namespace

VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx)
{
    // Register the last-write-wins authority handler at the Server/HostPeer.
    // On a Client node the handler is never invoked (the server holds authority);
    // registering it here is harmless — it is simply never called by the client's
    // NetworkManager::applyEdit(), which forwards edit intents instead.
    ctx->register_on_edit_received(ctx, on_edit_received, nullptr);

    // Register the pass-through authority policy validator.
    ctx->register_authority_policy(ctx, authority_policy, nullptr);

    return 0;  // success
}
