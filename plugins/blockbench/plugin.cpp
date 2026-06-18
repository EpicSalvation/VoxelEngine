// blockbench plugin — imports a Blockbench model (.bbmodel) into textured voxels
// (M15 T6). It plugs into the existing register_importer seam: the importer
// callback receives the file bytes plus, as user_data, the plugin's retained
// PluginContext, so from inside the import it can (a) decode each embedded texture
// and register it into the shared atlas (register_texture_data, T3), (b) register
// a material per model element whose faces point at those tiles (set_material_faces,
// T4), and (c) fill the voxel grid with those materials (the ImporterFn contract).
//
// Scope (per docs/m15-textured-voxels-audit.md): one-way import of the native
// .bbmodel JSON. Each face uses the WHOLE of its referenced texture image as its
// atlas tile — the separate-image-per-face authoring style (grass_top / grass_side
// / dirt). Per-face sub-UV regions within one shared sheet, and OBJ+MTL / Minecraft
// block-model variants, are left to a follow-on (the audit's decal/round-trip notes).
//
// No std:: type crosses the plugin ABI — the importer only calls the flat
// PluginContext function pointers. The JSON parser and base64 decoder below are
// self-contained so the plugin links nothing but glm (the engine build rule).

#include "plugin_api.h"
#include "world/Voxel.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#if defined(_WIN32)
#  define VOXEL_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define VOXEL_PLUGIN_EXPORT extern "C"
#endif

namespace {

// ── Minimal JSON parser (self-contained DOM) ────────────────────────────────
// Recursive-descent over a flat byte buffer. Supports objects, arrays, strings
// (with the common escapes), numbers, true/false/null — enough for .bbmodel.
struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool                              boolean = false;
    double                            number  = 0.0;
    std::string                       str;
    std::vector<JsonValue>            array;
    std::map<std::string, JsonValue>  object;

    bool isObject() const { return type == Type::Object; }
    bool isArray()  const { return type == Type::Array; }
    bool isNumber() const { return type == Type::Number; }

    // Object member access; returns nullptr when absent or not an object.
    const JsonValue* find(const std::string& key) const {
        if (type != Type::Object) return nullptr;
        auto it = object.find(key);
        return it == object.end() ? nullptr : &it->second;
    }
    double num(double fallback) const { return type == Type::Number ? number : fallback; }
};

class JsonParser {
public:
    JsonParser(const char* data, size_t size) : p_(data), end_(data + size) {}

    bool parse(JsonValue& out) {
        skipWs();
        if (!parseValue(out)) return false;
        // A well-formed top-level value is enough; trailing whitespace or bytes are
        // tolerated (some exporters append a newline), so success rides on parseValue.
        return true;
    }

private:
    const char* p_;
    const char* end_;

    void skipWs() {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
    }
    bool eof() const { return p_ >= end_; }

    bool parseValue(JsonValue& out) {
        skipWs();
        if (eof()) return false;
        switch (*p_) {
            case '{': return parseObject(out);
            case '[': return parseArray(out);
            case '"': out.type = JsonValue::Type::String; return parseString(out.str);
            case 't': case 'f': return parseBool(out);
            case 'n': return parseNull(out);
            default:  return parseNumber(out);
        }
    }

    bool parseObject(JsonValue& out) {
        out.type = JsonValue::Type::Object;
        ++p_;  // '{'
        skipWs();
        if (!eof() && *p_ == '}') { ++p_; return true; }
        while (!eof()) {
            skipWs();
            if (eof() || *p_ != '"') return false;
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (eof() || *p_ != ':') return false;
            ++p_;
            JsonValue val;
            if (!parseValue(val)) return false;
            out.object.emplace(std::move(key), std::move(val));
            skipWs();
            if (eof()) return false;
            if (*p_ == ',') { ++p_; continue; }
            if (*p_ == '}') { ++p_; return true; }
            return false;
        }
        return false;
    }

    bool parseArray(JsonValue& out) {
        out.type = JsonValue::Type::Array;
        ++p_;  // '['
        skipWs();
        if (!eof() && *p_ == ']') { ++p_; return true; }
        while (!eof()) {
            JsonValue val;
            if (!parseValue(val)) return false;
            out.array.push_back(std::move(val));
            skipWs();
            if (eof()) return false;
            if (*p_ == ',') { ++p_; continue; }
            if (*p_ == ']') { ++p_; return true; }
            return false;
        }
        return false;
    }

    bool parseString(std::string& out) {
        ++p_;  // opening quote
        out.clear();
        while (!eof()) {
            char c = *p_++;
            if (c == '"') return true;
            if (c == '\\') {
                if (eof()) return false;
                char e = *p_++;
                switch (e) {
                    case '"':  out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    case 'n':  out.push_back('\n'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'u':  // \uXXXX — keep ASCII, skip the 4 hex digits otherwise
                        if (end_ - p_ < 4) return false;
                        p_ += 4;
                        out.push_back('?');
                        break;
                    default: return false;
                }
            } else {
                out.push_back(c);
            }
        }
        return false;  // unterminated
    }

    bool parseNumber(JsonValue& out) {
        const char* start = p_;
        while (!eof() && (std::strchr("0123456789+-.eE", *p_) != nullptr)) ++p_;
        if (p_ == start) return false;
        out.type   = JsonValue::Type::Number;
        out.number = std::strtod(std::string(start, p_).c_str(), nullptr);
        return true;
    }

    bool parseBool(JsonValue& out) {
        if (end_ - p_ >= 4 && std::strncmp(p_, "true", 4) == 0) {
            p_ += 4; out.type = JsonValue::Type::Bool; out.boolean = true;  return true;
        }
        if (end_ - p_ >= 5 && std::strncmp(p_, "false", 5) == 0) {
            p_ += 5; out.type = JsonValue::Type::Bool; out.boolean = false; return true;
        }
        return false;
    }

    bool parseNull(JsonValue& out) {
        if (end_ - p_ >= 4 && std::strncmp(p_, "null", 4) == 0) {
            p_ += 4; out.type = JsonValue::Type::Null; return true;
        }
        return false;
    }
};

// ── base64 decode (for a "data:image/png;base64,...." texture source) ────────
int b64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;  // '=' padding or whitespace
}

std::vector<uint8_t> base64Decode(const std::string& in) {
    std::vector<uint8_t> out;
    int buf = 0, bits = 0;
    for (char c : in) {
        const int v = b64Value(c);
        if (v < 0) continue;  // skip padding/newlines
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buf >> bits) & 0xff));
        }
    }
    return out;
}

// Extract the base64 payload of a "data:...;base64,<payload>" URI; returns the
// whole string unchanged if it has no data-URI prefix (already raw base64).
std::string dataUriPayload(const std::string& source) {
    const size_t comma = source.find(',');
    if (source.rfind("data:", 0) == 0 && comma != std::string::npos)
        return source.substr(comma + 1);
    return source;
}

// ── Blockbench element → voxel-grid mapping ─────────────────────────────────
// Blockbench authors a block in a 16-unit cube space. Map a coordinate to a
// voxel index by scaling to grid_size and clamping. The element's [from,to] box
// fills the voxels it covers.
int toVoxel(double blockUnit, int grid_size) {
    const double scaled = blockUnit / 16.0 * static_cast<double>(grid_size);
    int v = static_cast<int>(std::floor(scaled + 1e-6));
    if (v < 0) v = 0;
    if (v > grid_size) v = grid_size;
    return v;
}

// Resolve a face's texture reference (an integer index into textures[]) to the
// texture_id this importer registered for that texture, or empty if unbound.
std::string faceTextureId(const JsonValue& face,
                          const std::vector<std::string>& textureIds) {
    const JsonValue* tex = face.find("texture");
    if (!tex || !tex->isNumber()) return std::string();
    const int idx = static_cast<int>(tex->number);
    if (idx < 0 || idx >= static_cast<int>(textureIds.size())) return std::string();
    return textureIds[static_cast<size_t>(idx)];
}

// Importer state: the assignment of palette indices to imported materials starts
// here, high enough to avoid the engine's low default palette slots.
constexpr uint8_t kImportPaletteBase = 200;

int importBlockbench(const uint8_t* file_data, size_t file_size,
                     WorldCoord /*anchor*/, int grid_size,
                     Voxel* out_voxels, void* user_data) {
    auto* ctx = static_cast<PluginContext*>(user_data);
    if (!ctx || !file_data || grid_size <= 0 || !out_voxels) return 1;

    JsonValue root;
    if (!JsonParser(reinterpret_cast<const char*>(file_data), file_size).parse(root) ||
        !root.isObject()) {
        return 2;  // malformed JSON → fail per the importer contract (no silent block)
    }

    const JsonValue* textures = root.find("textures");
    const JsonValue* elements = root.find("elements");
    if (!elements || !elements->isArray() || elements->array.empty())
        return 3;  // a model with no geometry is unsupported

    // (a) Register each embedded texture into the atlas; remember its texture_id.
    std::vector<std::string> textureIds;
    if (textures && textures->isArray()) {
        for (size_t i = 0; i < textures->array.size(); ++i) {
            const JsonValue& t = textures->array[i];
            const std::string id = "bb_tex_" + std::to_string(i);
            textureIds.push_back(id);
            const JsonValue* src = t.find("source");
            if (!src || src->type != JsonValue::Type::String) continue;
            const std::vector<uint8_t> png = base64Decode(dataUriPayload(src->str));
            if (!png.empty())
                ctx->register_texture_data(ctx, id.c_str(), png.data(), png.size());
        }
    }

    // Clear the target grid; only element-covered voxels become solid.
    const size_t total = static_cast<size_t>(grid_size) * grid_size * grid_size;
    for (size_t i = 0; i < total; ++i) out_voxels[i] = Voxel::empty();

    // (b)+(c) Each element → one material (palette index) with its face tiles, and
    // fill the element's box with that material.
    auto idx3 = [grid_size](int x, int y, int z) {
        return static_cast<size_t>(x) +
               static_cast<size_t>(grid_size) *
                   (static_cast<size_t>(y) + static_cast<size_t>(grid_size) * z);
    };

    bool placedAny = false;
    for (size_t e = 0; e < elements->array.size(); ++e) {
        const JsonValue& el = elements->array[e];
        const JsonValue* from = el.find("from");
        const JsonValue* to   = el.find("to");
        const JsonValue* faces = el.find("faces");
        if (!from || !from->isArray() || from->array.size() < 3 ||
            !to   || !to->isArray()   || to->array.size()   < 3)
            continue;

        const uint8_t palette_index =
            static_cast<uint8_t>(kImportPaletteBase + e);

        // Bind faces: top←up, bottom←down, side←north (the top/bottom/side model).
        if (faces && faces->isObject()) {
            std::string top, bottom, side;
            if (const JsonValue* f = faces->find("up"))    top    = faceTextureId(*f, textureIds);
            if (const JsonValue* f = faces->find("down"))  bottom = faceTextureId(*f, textureIds);
            if (const JsonValue* f = faces->find("north")) side   = faceTextureId(*f, textureIds);
            if (side.empty()) {  // fall back to any present lateral face
                for (const char* d : {"south", "east", "west"}) {
                    if (const JsonValue* f = faces->find(d)) {
                        side = faceTextureId(*f, textureIds);
                        if (!side.empty()) break;
                    }
                }
            }
            ctx->set_material_faces(ctx, palette_index,
                                    top.empty()    ? nullptr : top.c_str(),
                                    bottom.empty() ? nullptr : bottom.c_str(),
                                    side.empty()   ? nullptr : side.c_str(),
                                    /*tiling_factor*/ 1.0f);
        }

        const int x0 = toVoxel(from->array[0].num(0), grid_size);
        const int y0 = toVoxel(from->array[1].num(0), grid_size);
        const int z0 = toVoxel(from->array[2].num(0), grid_size);
        const int x1 = toVoxel(to->array[0].num(0), grid_size);
        const int y1 = toVoxel(to->array[1].num(0), grid_size);
        const int z1 = toVoxel(to->array[2].num(0), grid_size);

        Voxel v;
        v.material.palette_index = palette_index;
        v.material.density       = 1.0f;  // solid, removable default
        for (int z = z0; z < z1 && z < grid_size; ++z)
        for (int y = y0; y < y1 && y < grid_size; ++y)
        for (int x = x0; x < x1 && x < grid_size; ++x) {
            out_voxels[idx3(x, y, z)] = v;
            placedAny = true;
        }
    }

    return placedAny ? 0 : 4;  // nothing placed ⇒ unsupported/degenerate model
}

}  // namespace

VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    // Pass ctx as user_data so the importer can register textures/materials and
    // bind face tiles from inside the import (the ctx pointer is stable for the
    // plugin's lifetime — see plugin_api.h). One handler for the .bbmodel extension.
    ctx->register_importer(ctx, "bbmodel", &importBlockbench, ctx);
    return 0;
}
