// Built-in chat plugin (M11).
//
// Registers handlers for the "engine.chat" channel (Reliable, Broadcast) and
// fires console output on join/leave events. Provides a C-linkage helper
// `chat_send` that demos call to send a chat message.

#include "plugin_api.h"

#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#  define VOXEL_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define VOXEL_PLUGIN_EXPORT extern "C"
#endif
VOXEL_PLUGIN_ABI_STAMP();

static std::vector<std::string> g_chatLog;
static std::mutex               g_chatMutex;

static void append_log(const std::string& line)
{
    std::lock_guard<std::mutex> lk(g_chatMutex);
    g_chatLog.push_back(line);
    if (g_chatLog.size() > 64) g_chatLog.erase(g_chatLog.begin());
    std::cout << "[chat] " << line << '\n';
}

static void on_chat_message(const MessageEnvelope* env, void* /*user_data*/)
{
    if (!env || !env->payload || env->payload_size == 0) return;
    const std::string text(static_cast<const char*>(env->payload), env->payload_size);
    append_log("player:" + std::to_string(env->sender_id) + " > " + text);
}

static void on_joined(PlayerId id, WorldCoord /*pos*/, void* /*user_data*/)
{
    append_log("player:" + std::to_string(id) + " joined");
}

static void on_left(PlayerId id, void* /*user_data*/)
{
    append_log("player:" + std::to_string(id) + " left");
}

extern "C" void chat_send(PluginContext* ctx, const char* text)
{
    if (!ctx || !text) return;
    MessageEnvelope env{};
    env.channel_id    = "engine.chat";
    env.sender_id     = 0;
    env.target        = MessageTarget::Broadcast;
    env.target_player = 0;
    env.reliability   = MessageReliability::Reliable;
    env.payload       = text;
    env.payload_size  = std::strlen(text);
    ctx->send_network_message(ctx, &env);
}

extern "C" const std::vector<std::string>* chat_log() { return &g_chatLog; }

VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx)
{
    ctx->register_on_network_message(ctx, "engine.chat", on_chat_message, nullptr);
    ctx->register_on_player_joined(ctx, on_joined, nullptr);
    ctx->register_on_player_left(ctx, on_left, nullptr);
    return 0;
}
