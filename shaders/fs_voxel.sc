$input v_color0

// Minimal voxel fragment shader: output the interpolated vertex color.

#include <bgfx_shader.sh>

void main()
{
	gl_FragColor = v_color0;
}
