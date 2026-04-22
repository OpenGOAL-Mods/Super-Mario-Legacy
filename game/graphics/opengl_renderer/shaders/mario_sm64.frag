#version 410 core

in vec3 frag_color;
in vec2 frag_uv;

uniform sampler2D tex_T0;
// Output alpha multiplier.  MarioRenderer sets this to 1.0 so Mario stays
// opaque; SM64CollisionRenderer drops it to ~0.35 for the filled pass of
// the debug collision overlay so you can still see the world through it,
// then raises it back to 1.0 for the wireframe pass on top.
uniform float alpha;

out vec4 out_color;

void main() {
  // libsm64 geometry convention:
  //  - UVs with negative components mean "no texture, vertex color only" (hat fabric,
  //    overall fabric, skin, gloves, shoes).
  //  - UVs with positive components sample the atlas for decorative details such as the
  //    M hat logo, eyes, mustache, and overall buttons. The texture's alpha is the mask
  //    for blending the atlas detail on top of the base vertex color.
  vec3 color = frag_color;
  if (frag_uv.x >= 0.0 && frag_uv.y >= 0.0) {
    vec4 tex = texture(tex_T0, frag_uv);
    color = mix(frag_color, tex.rgb, tex.a);
  }
  out_color = vec4(color, alpha);
}
