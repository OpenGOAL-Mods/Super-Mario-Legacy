#version 410 core

in vec3 frag_color;
in vec2 frag_uv;

uniform sampler2D tex_T0;
// Output alpha multiplier.  MarioRenderer sets this to 1.0 so Mario stays
// opaque; SM64CollisionRenderer drops it to ~0.35 for the filled pass of
// the debug collision overlay so you can still see the world through it,
// then raises it back to 1.0 for the wireframe pass on top.
uniform float alpha;
// Target color for Mario's red fabric.  Used as a hue source: the shader
// extracts the hue of u_tint and rotates each red-detected pixel to that
// hue, preserving the pixel's saturation + value.  Vanilla red = (1,0,0)
// means "rotate by 0°" so the model looks identical to the original.
// The Mario debug menu's Colors submenu (mario-debug-menu.gc) writes the
// preset RGBs.
uniform vec3 u_tint;

out vec4 out_color;

// Standard GLSL RGB↔HSV (Sam Hocevar's branch-free formulation).
vec3 rgb2hsv(vec3 c) {
  vec4 K = vec4(0.0, -1.0/3.0, 2.0/3.0, -1.0);
  vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
  vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
  float d = q.x - min(q.w, q.y);
  float e = 1.0e-10;
  return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}
vec3 hsv2rgb(vec3 c) {
  vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
  vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
  return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

// If `c` is "red enough" (hue near 0, saturated, not pitch-black), rotate
// its hue to `target_h` while keeping saturation and value.  Otherwise
// return `c` unchanged.  Detection in HSV catches Mario's red across the
// full brightness range (highlights, shadows, antialiased edges) without
// false-positives on skin/brown/pink, which all live further down the
// hue circle (~0.05+) and would fail the 0.03 hue threshold.
vec3 shift_red_hue(vec3 c, float target_h) {
  vec3 hsv = rgb2hsv(c);
  // Hue is in [0,1); red wraps so we accept both ends.
  bool red_hue = (hsv.x < 0.03) || (hsv.x > 0.97);
  // Saturation > 0.5 rejects washed-out near-grays; value > 0.1 rejects
  // black AA seams.  Mario's red, even in shadow, sits well above both.
  bool saturated = (hsv.y > 0.5) && (hsv.z > 0.1);
  if (red_hue && saturated) {
    hsv.x = target_h;
    return hsv2rgb(hsv);
  }
  return c;
}

void main() {
  // libsm64 geometry convention:
  //  - UVs with negative components mean "no texture, vertex color only" (hat fabric,
  //    overall fabric, skin, gloves, shoes).
  //  - UVs with positive components sample the atlas for decorative details such as the
  //    M hat logo, eyes, mustache, and overall buttons. The texture's alpha is the mask
  //    for blending the atlas detail on top of the base vertex color.

  // Target hue from u_tint.  rgb2hsv((1,0,0)) = hue 0; the Red preset
  // therefore rotates by 0° → vanilla appearance.  We also hard-skip the
  // recolor path entirely when target_h sits at the red extremes,
  // guaranteeing bit-identical output to vanilla even on slightly-off-red
  // atlas texels.
  float target_h = rgb2hsv(u_tint).x;
  bool recolor = (target_h > 0.005) && (target_h < 0.995);

  // Apply hue shift to BOTH signals so red vertex color (fabric, sleeves)
  // AND red texture pixels (the front-of-hat M-logo badge background)
  // both get the same hue rotation.
  vec3 base = recolor ? shift_red_hue(frag_color, target_h) : frag_color;

  vec3 color = base;
  if (frag_uv.x >= 0.0 && frag_uv.y >= 0.0) {
    vec4 tex = texture(tex_T0, frag_uv);
    vec3 tex_rgb = recolor ? shift_red_hue(tex.rgb, target_h) : tex.rgb;
    color = mix(base, tex_rgb, tex.a);
  }
  out_color = vec4(color, alpha);
}
