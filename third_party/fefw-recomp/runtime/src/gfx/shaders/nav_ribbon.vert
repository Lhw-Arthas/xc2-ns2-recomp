// SPDX-License-Identifier: GPL-3.0-or-later
#version 450

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;    // x = [-1, +1] across width, y = meters along path
layout(location = 2) in float in_fade; // near/far alpha envelope [0, 1]

layout(push_constant) uniform PC {
    mat4 vp;
    vec4 color;      // rgb = base tint, a = master opacity
    vec4 anim;       // x = time_sec, y = reveal_dist_m, z = occluded_alpha, w = has_depth
    vec4 depth_info; // x = 1/W, y = 1/H, z = depth_flip_y, w = depth_reverse_z
} pc;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out float v_fade;

void main() {
    vec4 clip = pc.vp * vec4(in_pos, 1.0);
    // Support both NVN_DEPTH_MODE_NEAR_IS_ZERO (0/1) and NEAR_IS_MINUS_W (2/3)
    float z = (pc.depth_info.w >= 1.5) ? (clip.z + clip.w) * 0.5 : clip.z;
    // XC2 m_viewProj is Y-up; Vulkan NDC is Y-down.
    gl_Position = vec4(clip.x, -clip.y, z, clip.w);
    v_uv = in_uv;
    v_fade = in_fade;
}
