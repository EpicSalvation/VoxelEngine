$input v_color0, v_texcoord0, v_texcoord1, v_fogdist

// Voxel fragment shader: wrap the tile-local UV into the bound tile's atlas
// sub-rect and modulate by the interpolated vertex color (which carries the
// per-face shade and translucency), then blend toward the distance-obscurance
// fog color (M17).
//
// v_texcoord0 is a TILE-LOCAL coordinate the mesh builder scaled by
// face_world_size × tiling_factor, so frac() makes the tile REPEAT across a large
// face at a fixed world density — hardware REPEAT cannot wrap a sub-rectangle of
// an atlas, so the wrap lives here (M15 T5). v_texcoord1 is the tile's atlas
// sub-rect (u0,v0,u1,v1). The atlas is point-sampled, so a wrapped coordinate at
// a seam lands on the tile's own edge texel — no bleed into the neighbor tile.
//
// With the default 1×1 white atlas, the full-atlas sub-rect (0,0,1,1), and UV
// (0,0) — the state until content binds per-face tiles (T4/T5) — the sample
// returns white and the output equals the vertex color, so colored worlds render
// byte-identically to the pre-texture pipeline.
//
// Distance obscurance: u_fogParams = (near, far, density, _); u_fogColor.rgb is
// the color geometry fades into. The fog factor is a linear near→far ramp clamped
// to [0,1] and scaled by density — the exact curve CPU-side fogFactor() mirrors
// (include/renderer/Fog.h). density 0 (the default) → factor 0 → mix() returns the
// un-fogged fragment exactly, so an un-fogged scene is byte-identical. Only RGB is
// fogged; the fragment's alpha (translucency) is preserved.

#include <bgfx_shader.sh>

SAMPLER2D(s_atlas, 0);

uniform vec4 u_fogColor;   // rgb = fog color (a unused)
uniform vec4 u_fogParams;  // x = near (m), y = far (m), z = density [0,1], w unused

void main()
{
	vec2 base = v_texcoord1.xy;                 // tile u0,v0 in the atlas
	vec2 size = v_texcoord1.zw - v_texcoord1.xy;// tile width,height in the atlas
	vec2 uv   = base + fract(v_texcoord0) * size;
	vec4 scene = texture2D(s_atlas, uv) * v_color0;

	float fog = clamp((v_fogdist - u_fogParams.x) / max(u_fogParams.y - u_fogParams.x, 1e-4), 0.0, 1.0) * u_fogParams.z;
	gl_FragColor = vec4(mix(scene.rgb, u_fogColor.rgb, fog), scene.a);
}
