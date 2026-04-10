#version 410 core

layout (location = 0) in vec3 position_in;
layout (location = 1) in vec3 normal_in;
layout (location = 2) in vec3 color_in;
layout (location = 3) in vec2 uv_in;

uniform vec4 hvdf_offset;
uniform mat4 camera;
uniform float fog_constant;
uniform vec3 light_dir;

out vec3 frag_color;
out vec2 frag_uv;

void main() {
  // Camera transform (same as collision shader)
  vec4 transformed = -camera[3].xyzw;
  transformed += -camera[0] * position_in.x;
  transformed += -camera[1] * position_in.y;
  transformed += -camera[2] * position_in.z;

  // Compute Q for perspective
  float Q = fog_constant / transformed[3];

  // Perspective divide
  transformed.xyz *= Q;

  // Offset
  transformed.xyz += hvdf_offset.xyz;

  // Correct xy offset
  transformed.xy -= (2048.);

  // Correct z scale
  transformed.z /= (8388608);
  transformed.z -= 1;

  // Correct xy scale
  transformed.x /= (256);
  transformed.y /= -(128);

  // Hack
  transformed.xyz *= transformed.w;

  gl_Position = transformed;
  // Scissoring area adjust
  gl_Position.y *= SCISSOR_ADJUST * HEIGHT_SCALE;

  // Simple directional lighting on Mario
  float ndotl = max(dot(normal_in, light_dir), 0.0);
  float ambient = 0.4;
  float diffuse = 0.6 * ndotl;

  frag_color = color_in * (ambient + diffuse);
  frag_uv = uv_in;
}
