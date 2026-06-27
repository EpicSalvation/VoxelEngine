$input a_position, a_color0, a_texcoord0, a_texcoord1
$output v_color0, v_texcoord0, v_texcoord1, v_fogdist

// Minimal voxel vertex shader: transform by the model-view-projection matrix
// and pass through the per-vertex color, the tile-local atlas UV (texcoord0),
// and the bound tile's atlas sub-rect (texcoord1 = u0,v0,u1,v1). The model
// matrix carries the floating-origin (camera-local) translation set per voxel on
// the CPU side. The fragment shader wraps the in-tile UV into the sub-rect.
//
// v_fogdist is the camera-forward distance (depth) to the vertex, the input to the
// fragment shader's distance-obscurance fog (M17). It is the perspective `w` of the
// clip-space position — for bx::mtxProj, |clip.w| == the view-space depth in metres
// — which is always present (no dependence on a predefined u_modelView the bytecode
// optimizer might drop). gl_Position is computed exactly as before, so a scene with
// fog disabled is byte-identical; the distance is a separate output.

#include <bgfx_shader.sh>

void main()
{
	vec4 clip = mul(u_modelViewProj, vec4(a_position, 1.0) );
	gl_Position = clip;
	v_color0 = a_color0;
	v_texcoord0 = a_texcoord0;
	v_texcoord1 = a_texcoord1;
	v_fogdist = abs(clip.w);
}
