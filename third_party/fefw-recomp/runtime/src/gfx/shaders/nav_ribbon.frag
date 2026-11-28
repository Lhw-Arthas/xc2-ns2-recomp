// SPDX-License-Identifier: GPL-3.0-or-later
#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 1) in float v_fade;

layout(set = 0, binding = 0) uniform sampler2D u_depth;

layout(push_constant) uniform PC {
    mat4 vp;
    vec4 color;      // rgb = base tint, a = master opacity
    vec4 anim;       // x = time_sec, y = reveal_dist_m, z = occluded_alpha, w = has_depth
    vec4 depth_info; // x = 1/W, y = 1/H, z = depth_flip_y, w = depth_reverse_z
} pc;

layout(location = 0) out vec4 out_color;

void main() {
    float u = v_uv.x;        // [-1, +1] across ribbon width
    float au = abs(u);
    float dist_m = v_uv.y;   // meters along path from player

    // 0.45s sweep-out reveal animation from player's feet
    float reveal = smoothstep(pc.anim.y, pc.anim.y - 5.5, dist_m);
    float env = v_fade * reveal;
    if (env <= 0.004) discard;

    // 1. Soft translucent conveyor-belt body + subtle glowing side rails
    float body_mask = 1.0 - smoothstep(0.52, 1.0, au);
    float rail = exp(-pow((au - 0.78) / 0.11, 2.0)) * 0.55;
    float spine = exp(-pow(au / 0.24, 2.0)) * 0.22;

    // 2. Scrolling >>> conveyor chevrons pointing forward along +dist_m
    //    V-shape: center leads ahead of edges -> chev_phase = dist_m + au * 0.85 - time * speed
    float speed = 5.8;      // meters / sec
    float spacing = 2.4;    // meters between chevrons
    float ph = fract((dist_m + au * 0.82 - pc.anim.x * speed) / spacing);
    // Sharp leading edge, smooth trailing tail (classic XC3/Forza conveyor belt look)
    float chev_head = smoothstep(0.0, 0.10, ph) * smoothstep(0.48, 0.10, ph);
    float chev_mask = chev_head * (1.0 - smoothstep(0.62, 0.94, au));

    // Gentle travelling aether shimmer along the ribbon
    float pulse = 0.86 + 0.14 * sin(dist_m * 0.45 - pc.anim.x * 3.8);

    // Combine base translucent belt + glowing rails + bright scrolling chevrons
    float base_a = (body_mask * 0.30 + rail * 0.45 + spine) * pulse;
    float chev_a = chev_mask * 0.68;
    float alpha = clamp((base_a + chev_a) * pc.color.a * env, 0.0, 0.92);

    // Color: rich tint on belt body, near-white bright core on chevrons & rails
    vec3 tint = pc.color.rgb;
    vec3 bright = mix(tint, vec3(0.95, 0.99, 1.0), 0.72);
    vec3 rgb = mix(tint, bright, clamp(chev_mask * 0.85 + rail * 0.40 + spine * 0.35, 0.0, 1.0));

    // 3. Read-only depth occlusion against the game's native 3D G-buffer depth
    if (pc.anim.w > 0.5) {
        vec2 suv = gl_FragCoord.xy * pc.depth_info.xy;
        if (pc.depth_info.z > 0.5) suv.y = 1.0 - suv.y;
        float scene_z = texture(u_depth, clamp(suv, vec2(0.001), vec2(0.999))).r;
        float frag_z = gl_FragCoord.z;
        // Bias slightly toward camera so the lifted ground ribbon never z-fights with grass/slopes
        bool rev_z = (pc.depth_info.w > 0.5 && pc.depth_info.w < 1.5) || (pc.depth_info.w > 2.5);
        bool occluded = rev_z
            ? (frag_z < scene_z - 0.00018)   // reverse-Z: larger = closer
            : (frag_z > scene_z + 0.00018);  // forward-Z: smaller = closer
        if (occluded) {
            alpha *= pc.anim.z;  // soft 14% translucent silhouette behind hills/rocks/player
            rgb *= 0.85;
        }
    }

    if (alpha <= 0.004) discard;
    out_color = vec4(rgb, alpha);
}
