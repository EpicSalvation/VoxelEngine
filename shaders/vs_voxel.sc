$input a_position, a_color0
$output v_color0

// Minimal voxel vertex shader: transform by the model-view-projection matrix
// and pass the per-vertex color through. The model matrix carries the
// floating-origin (camera-local) translation set per voxel on the CPU side.

#include <bgfx_shader.sh>

void main()
{
	gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0) );
	v_color0 = a_color0;
}
