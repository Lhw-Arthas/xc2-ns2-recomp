# Compiles the output pass shaders (GLSL below) with glslc (Vulkan SDK), validates each with spirv-val, and writes
# src/gfx/extras/extras_shaders.inc (uint32 arrays). The .inc is committed, so building the runtime does not need the SDK.
# Run: python -I scripts/gen_extras_shaders.py   (exit 1 and no .inc written if any shader fails to compile or validate)
#
# The output pass encodes the game's final picture for the swapchain (sRGB, HDR10 or scRGB) and optionally upscales it with
# AMD FidelityFX FSR 1 (EASU then RCAS, MIT, headers in third_party/ffx_fsr1, the SDK v1.1.4 copy; see its README.txt).
# The layouts (push constants and descriptors) are documented in the header comment that this script writes into the .inc.
import os, shutil, struct, subprocess, sys, tempfile

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
glslc = r"C:\VulkanSDK\1.4.363.0\Bin\glslc.exe"
spirv_val = r"C:\VulkanSDK\1.4.363.0\Bin\spirv-val.exe"
fsr_inc = os.path.join(root, "third_party", "ffx_fsr1")
out_path = os.path.join(root, "src", "gfx", "extras", "extras_shaders.inc")

# Vertex shader: the same full-screen triangle as scripts/gen_present_shaders.py (VERT there).
OUT_VERT = r"""#version 450
// Full-screen triangle. v_dst is the destination position in [0,1]^2 (0,0 top left, as the image rows run).
layout(location = 0) out vec2 v_dst;
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_dst = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
"""

# Output encode: present.frag of gen_present_shaders.py plus the fourth push constant p (the mode and the nit levels).
OUT_ENCODE_FRAG = r"""#version 450
layout(location = 0) in vec2 v_dst;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_src;
layout(set = 0, binding = 1) uniform sampler2D u_scene;   // the window texture before the UI was drawn (u_final when unknown)
layout(set = 0, binding = 2) uniform sampler2D u_final;   // the window texture as presented, at the size of u_scene
layout(push_constant) uniform PushConstants {
    vec4 pic;    // the picture rectangle in destination UV: origin xy, size zw (0,0,1,1 when the picture fills the target)
    vec4 crop;   // the source crop in texels: origin xy, size zw
    vec4 src;    // xy: the whole source size in texels, z: 1 = flip the picture vertically,
                 // w: highlight expansion peak ratio (> 1.0 expands display-referred [0,1] highlights toward peak_nits in HDR modes)
    vec4 p;      // x: mode (0 copy, 1 linear to sRGB, 2 linear to HDR10, 3 linear to scRGB), y: paper white nits, z: peak nits,
                 // w: 1 = apply the picture controls in adj
    vec4 adj;    // picture controls: x contrast, y saturation, z black level, w gamma (neutral 1, 1, 0, 1)
    vec4 scn;    // the picture in u_scene / u_final UV: origin xy, size zw
    vec4 ui;     // x: UI brightness over paper white (1 = paper white), y: 1 = deband the 8-bit source (HDR modes),
                 // z: HDR test pattern (0 none, 1 peak, 2 black, 3 staircase), w: the peak pattern's inner squares in nits
    vec4 hx;     // x: 1 = u_src (and u_scene / u_final) hold display-encoded floats with highlights above 1, y: the brightest highlight
                 // over paper white they are fitted to,
                 // w: 1 = clip the source at 1 instead (the 8-bit look, for the highlight expansion of src.w)
} pc;

// Picture controls on a display-encoded signal (0..1 in SDR; above 1 for HDR highlights): black level, contrast around mid grey, saturation
// around the Rec. 709 luma of the signal, then gamma. White (1.0) stays where it is for the black level and the gamma.
vec3 picture(vec3 c) {
    float b = pc.adj.z;
    c = b < 0.0 ? (c + b) / (1.0 + b) : b + c * (1.0 - b);
    c = (c - 0.5) * pc.adj.x + 0.5;
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = vec3(l) + (c - vec3(l)) * pc.adj.y;
    return pow(max(c, vec3(0.0)), vec3(1.0 / pc.adj.w));
}

// IEC 61966-2-1 sRGB OETF of clamp(x, 0, 1).
vec3 srgb_oetf(vec3 x) {
    x = clamp(x, 0.0, 1.0);
    vec3 lo = x * 12.92;
    vec3 hi = 1.055 * pow(x, vec3(1.0 / 2.4)) - 0.055;
    return vec3(x.r <= 0.0031308 ? lo.r : hi.r, x.g <= 0.0031308 ? lo.g : hi.g, x.b <= 0.0031308 ? lo.b : hi.b);
}

// IEC 61966-2-1 sRGB EOTF of clamp(x, 0, 1).
vec3 srgb_eotf(vec3 x) {
    x = clamp(x, 0.0, 1.0);
    vec3 lo = x / 12.92;
    vec3 hi = pow((x + 0.055) / 1.055, vec3(2.4));
    return vec3(x.r <= 0.04045 ? lo.r : hi.r, x.g <= 0.04045 ? lo.g : hi.g, x.b <= 0.04045 ? lo.b : hi.b);
}

// A display-encoded value with highlights above 1 to linear: sRGB up to 1, a 2.2 power above (the tone-map patch's encode).
vec3 decode_ext(vec3 x) {
    x = max(x, vec3(0.0));
    vec3 hi = pow(x, vec3(2.2));
    vec3 lo = srgb_eotf(x);
    return vec3(x.r > 1.0 ? hi.r : lo.r, x.g > 1.0 ? hi.g : lo.g, x.b > 1.0 ? hi.b : lo.b);
}

// Highlights above paper white (1.0) brought under r: slope 1 at 1, r the asymptote; one factor for the three channels keeps the hue.
vec3 fit_highlights(vec3 c, float r) {
    float m = max(max(c.r, c.g), c.b);
    if (m <= 1.0) return c;
    float a = max(r - 1.0, 1e-4);
    return c * ((1.0 + a * (1.0 - exp(-(m - 1.0) / a))) / m);
}

// Triangular noise in [-1, 1] for a pixel (the sum of two hashed uniforms), the same every frame.
float tpdf_noise(uvec2 p) {
    uint h = p.x * 1664525u + p.y * 22695477u + 1013904223u;
    h ^= h >> 16; h *= 2246822519u; h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;
    uint g = h * 747796405u + 2891336453u;
    g ^= g >> 16; g *= 2246822519u; g ^= g >> 13;
    return float(h >> 8) / 16777216.0 + float(g >> 8) / 16777216.0 - 1.0;
}

// SMPTE ST 2084 (PQ) inverse EOTF: nits in [0, 10000] to the 0..1 signal.
const float PQ_M1 = 2610.0 / 16384.0;
const float PQ_M2 = 2523.0 / 4096.0 * 128.0;
const float PQ_C1 = 3424.0 / 4096.0;
const float PQ_C2 = 2413.0 / 4096.0 * 32.0;
const float PQ_C3 = 2392.0 / 4096.0 * 32.0;
vec3 pq_encode(vec3 nits) {
    vec3 y = clamp(nits / 10000.0, 0.0, 1.0);
    vec3 ym = pow(y, vec3(PQ_M1));
    return pow((vec3(PQ_C1) + PQ_C2 * ym) / (vec3(1.0) + PQ_C3 * ym), vec3(PQ_M2));
}

void main() {
    vec2 local = (v_dst - pc.pic.xy) / pc.pic.zw;
    if (any(lessThan(local, vec2(0.0))) || any(greaterThan(local, vec2(1.0)))) {
        o_color = vec4(0.0, 0.0, 0.0, 1.0);   // letterbox bar (black in every mode)
        return;
    }
    if (pc.src.z > 0.5) local.y = 1.0 - local.y;   // flip: a window with the lower-left origin mode is presented upside down otherwise
    // texel-space position inside the crop; clamped to the crop's texel centres so a linear filter never reads outside it
    vec2 texel = pc.crop.xy + local * pc.crop.zw;
    texel = clamp(texel, pc.crop.xy + 0.5, pc.crop.xy + pc.crop.zw - 0.5);
    vec3 rgb = textureLod(u_src, texel / pc.src.xy, 0.0).rgb;
    bool enc = pc.hx.x > 0.5, clip = pc.hx.w > 0.5;   // clip: the old look, as if the source were 8-bit
    if (enc) rgb = clip ? srgb_eotf(rgb) : decode_ext(rgb);

    int mode = int(pc.p.x + 0.5);
    bool adjust = pc.p.w > 0.5;
    // HDR test patterns (extras/hdr_pattern.h): square tiles across the picture
    int pat = mode >= 2 ? int(pc.ui.z + 0.5) : 0;
    if (pat != 0) {
        vec2 t = local * vec2(8.0, 4.5) + vec2(0.0, 0.25);
        vec2 d = abs(fract(t) - 0.5);
        float m = max(d.x, d.y);
        if (pat == 3) {   // staircase: a strip along the top, 8 bars of 300 to 1000 nits in steps of 100, on black; no peak limit
            float bx = local.x * 8.0;
            float f = fract(bx);
            float nits = (local.y < 0.07 && f > 0.04 && f < 0.96) ? 300.0 + 100.0 * floor(bx) : 0.0;
            o_color = vec4(mode == 2 ? pq_encode(vec3(nits)) : vec3(nits / 80.0), 1.0);
            return;
        }
        if (pat == 1) {   // peak: the peak being tried inside 10000 nits, on black, about 7% of the screen bright (an OLED dims more); no peak limit
            float nits = m < 0.07 ? pc.ui.w : (m < 0.13 ? 10000.0 : 0.0);
            o_color = vec4(mode == 2 ? pq_encode(vec3(nits)) : vec3(nits / 80.0), 1.0);
            return;
        }
        // black: 8-bit codes 1 to 16 on black, then the picture controls and the encode like the game's picture
        const float codes[8] = float[8](1.0, 2.0, 3.0, 4.0, 6.0, 8.0, 12.0, 16.0);
        ivec2 c = ivec2(floor(t));
        rgb = srgb_eotf(vec3(m < 0.3 ? codes[(c.x + c.y * 3) % 8] / 255.0 : 0.0));
    }
    // HDR stretches the 8-bit picture, so its steps can show as bands: one step of noise on the sRGB code, none at black or white
    if (mode >= 2 && pc.ui.y > 0.5 && pat == 0) {
        vec3 e = srgb_oetf(rgb);
        vec3 room = clamp(min(e, 1.0 - e) * 255.0, 0.0, 1.0);
        vec3 d = srgb_eotf(e + room * (tpdf_noise(uvec2(gl_FragCoord.xy)) / 255.0));
        rgb = vec3(rgb.r < 1.0 ? d.r : rgb.r, rgb.g < 1.0 ? d.g : rgb.g, rgb.b < 1.0 ? d.b : rgb.b);   // highlights above white as they are
    }
    if (adjust && mode >= 2) rgb = pow(picture(pow(max(rgb, vec3(0.0)), vec3(1.0 / 2.2))), vec3(2.2));   // HDR: on a 2.2 power curve
    if (enc && !clip && mode >= 2 && pat == 0) rgb = fit_highlights(rgb, pc.hx.y);   // the game's own highlights, fitted to the display
    // Display-referred 8-bit game output is clamped to [0,1] (= [0, paper white]). When highlight expansion is requested,
    // keep shadows/midtones at 1:1 paper white and lift upper highlights smoothly toward peak_ratio * paper_white.
    if (pat == 0 && mode >= 2 && (pc.src.w > 1.001 || abs(pc.ui.x - 1.0) > 0.001)) {
        rgb = max(rgb, vec3(0.0));
        float gain = 1.0;
        if (pc.src.w > 1.001) {
            float l = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
            float m = max(max(rgb.r, rgb.g), rgb.b);
            float driver = mix(l, m, 0.5);
            float u = clamp((driver - 0.25) / 0.75, 0.0, 1.0);
            gain = 1.0 + (pc.src.w - 1.0) * pow(u, 2.2);
        }
        // UI drawn over the scene changed the pixel: no expansion, the UI brightness instead
        vec2 suv = pc.scn.xy + local * pc.scn.zw;
        vec3 fin = max(textureLod(u_final, suv, 0.0).rgb, vec3(0.0)), scn = max(textureLod(u_scene, suv, 0.0).rgb, vec3(0.0));
        if (enc) { fin = clip ? srgb_eotf(fin) : decode_ext(fin); scn = clip ? srgb_eotf(scn) : decode_ext(scn); }
        vec3 d = abs(pow(fin, vec3(1.0 / 2.2)) - pow(scn, vec3(1.0 / 2.2)));
        float ui = smoothstep(0.008, 0.04, max(d.r, max(d.g, d.b)));
        rgb *= mix(gain, pc.ui.x, ui);
    }
    vec3 outc;
    if (mode == 1) {
        outc = srgb_oetf(rgb);
        if (adjust) outc = clamp(picture(outc), 0.0, 1.0);
    } else if (mode == 2) {
        // scene-linear BT.709 with 1.0 = paper white -> BT.2020 -> nits, limited to the peak, then PQ
        vec3 r2020 = vec3(dot(vec3(0.6274, 0.3293, 0.0433), rgb),
                          dot(vec3(0.0691, 0.9195, 0.0114), rgb),
                          dot(vec3(0.0164, 0.0880, 0.8956), rgb));
        vec3 nits = clamp(min(r2020 * pc.p.y, vec3(pc.p.z)), 0.0, 10000.0);
        outc = pq_encode(nits);
    } else if (mode == 3) {
        outc = min(rgb * pc.p.y, vec3(pc.p.z)) / 80.0;   // scRGB: 1.0 = 80 nits, limited to the peak like HDR10
    } else {
        outc = rgb;                  // mode 0 (and any other value): the texel unchanged
        if (adjust) outc = clamp(picture(outc), 0.0, 1.0);
    }
    o_color = vec4(outc, 1.0);
}
"""

# Overlay composite: the ImGui overlay (rendered into an RGBA8 UNORM image, premultiplied alpha, 1.0 = UI white) drawn on top of an HDR
# swapchain image. UI white maps to paper white; the colours are encoded like kOutEncodeFrag modes 2 and 3.
OVERLAY_COMPOSITE_FRAG = r"""#version 450
layout(location = 0) in vec2 v_dst;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_ui;
layout(push_constant) uniform PushConstants {
    vec4 p;      // x: mode (1 SDR, 2 HDR10, 3 scRGB), y: paper white nits, z: peak nits
} pc;

// IEC 61966-2-1 sRGB EOTF (encoded to linear) of clamp(x, 0, 1).
vec3 srgb_eotf(vec3 x) {
    x = clamp(x, 0.0, 1.0);
    vec3 lo = x / 12.92;
    vec3 hi = pow((x + 0.055) / 1.055, vec3(2.4));
    return vec3(x.r <= 0.04045 ? lo.r : hi.r, x.g <= 0.04045 ? lo.g : hi.g, x.b <= 0.04045 ? lo.b : hi.b);
}

// SMPTE ST 2084 (PQ) inverse EOTF: nits in [0, 10000] to the 0..1 signal.
const float PQ_M1 = 2610.0 / 16384.0;
const float PQ_M2 = 2523.0 / 4096.0 * 128.0;
const float PQ_C1 = 3424.0 / 4096.0;
const float PQ_C2 = 2413.0 / 4096.0 * 32.0;
const float PQ_C3 = 2392.0 / 4096.0 * 32.0;
vec3 pq_encode(vec3 nits) {
    vec3 y = clamp(nits / 10000.0, 0.0, 1.0);
    vec3 ym = pow(y, vec3(PQ_M1));
    return pow((vec3(PQ_C1) + PQ_C2 * ym) / (vec3(1.0) + PQ_C3 * ym), vec3(PQ_M2));
}

void main() {
    vec4 texel = texture(u_ui, v_dst);
    float a = texel.a;
    if (a <= 0.0) {
        o_color = vec4(0.0);   // nothing drawn here: the blend leaves the swapchain image unchanged
        return;
    }
    vec3 straight = clamp(texel.rgb / a, 0.0, 1.0);
    vec3 lin = srgb_eotf(straight);   // scene-linear, 1.0 = paper white
    int mode = int(pc.p.x + 0.5);
    vec3 outc;
    if (mode == 1) {
        outc = straight;              // SDR (the HDR / SDR compare): the sRGB-encoded colour as ImGui would write it
    } else if (mode == 2) {
        // BT.709 to BT.2020 -> nits, limited to the peak, then PQ
        vec3 r2020 = vec3(dot(vec3(0.6274, 0.3293, 0.0433), lin),
                          dot(vec3(0.0691, 0.9195, 0.0114), lin),
                          dot(vec3(0.0164, 0.0880, 0.8956), lin));
        vec3 nits = clamp(min(r2020 * pc.p.y, vec3(pc.p.z)), 0.0, 10000.0);
        outc = pq_encode(nits);
    } else if (mode == 3) {
        outc = lin * pc.p.y / 80.0;   // scRGB: 1.0 = 80 nits, not clamped
    } else {
        outc = lin;                   // any other value: scene-linear, unencoded
    }
    o_color = vec4(outc, a);
}
"""

# The SDK headers are read as GPU GLSL with 32-bit maths (FFX_HALF 0). FSR_RCAS_DENOISE is deliberately not defined.
FSR_COMMON_DEFINES = """#extension GL_GOOGLE_include_directive : require
#define FFX_GPU 1
#define FFX_GLSL 1
#define FFX_HALF 0
"""

# FSR 1 EASU, driven the way the SDK's ffx_fsr1_easu.h drives it (64 invocations, 16x16 tile, four pixels each).
FSR_EASU_COMP = "#version 450\n" + FSR_COMMON_DEFINES + r"""#include "ffx_core.h"
#define FFX_FSR_EASU_FLOAT 1

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 0) uniform sampler2D u_src;                       // linear filter, clamp to edge
layout(set = 0, binding = 1, rgba16f) writeonly uniform image2D u_dst;
layout(push_constant) uniform PushConstants {
    uvec4 con0;   // fsr1_easu_consts() output, in this order
    uvec4 con1;
    uvec4 con2;
    uvec4 con3;
    vec4 opt;     // x: 1 = scene-linear HDR input (compress/expand), 0 = [0,1] input; y, z: output width, height; w: pre-scale k of the compression (0 = 1)
} pc;

bool linear_in() { return pc.opt.x > 0.5; }

// Perceptual compression for scene-linear input: c = k*x / (1 + k*x) with k = opt.w (0 means 1), so FSR sees values in [0, 1).
float prescale() { return pc.opt.w > 0.0 ? pc.opt.w : 1.0; }

vec4 compress4(vec4 x) {
    x = max(x, vec4(0.0)) * prescale();
    return x / (vec4(1.0) + x);
}

// Undo the compression after EASU: x = c / (1 - c) / k, with c clamped to [0, 0.999].
vec3 expand3(vec3 c) {
    c = clamp(c, vec3(0.0), vec3(0.999));
    return c / (vec3(1.0) - c) / prescale();
}

// The fetch callbacks of the SDK's ffx_fsr1_callbacks_glsl.h, on this pass's bindings.
FfxFloat32x4 FsrEasuRF(FfxFloat32x2 p) { vec4 r = textureGather(u_src, p, 0); return linear_in() ? compress4(r) : r; }
FfxFloat32x4 FsrEasuGF(FfxFloat32x2 p) { vec4 r = textureGather(u_src, p, 1); return linear_in() ? compress4(r) : r; }
FfxFloat32x4 FsrEasuBF(FfxFloat32x2 p) { vec4 r = textureGather(u_src, p, 2); return linear_in() ? compress4(r) : r; }

#include "fsr1/ffx_fsr1.h"

void CurrFilter(uvec2 pos) {
    if (any(greaterThanEqual(pos, uvec2(pc.opt.yz)))) return;   // outside the output
    vec3 c;
    ffxFsrEasuFloat(c, pos, pc.con0, pc.con1, pc.con2, pc.con3);
    if (linear_in()) c = expand3(c);
    imageStore(u_dst, ivec2(pos), vec4(c, 1.0));
}

void main() {
    // Remap the 64 invocations to a PS-like 8x8 swizzle, then four pixels per invocation: a 16x16 tile.
    uvec2 gxy = ffxRemapForQuad(gl_LocalInvocationID.x) + uvec2(gl_WorkGroupID.x << 4u, gl_WorkGroupID.y << 4u);
    CurrFilter(gxy);
    gxy.x += 8u;
    CurrFilter(gxy);
    gxy.y += 8u;
    CurrFilter(gxy);
    gxy.x -= 8u;
    CurrFilter(gxy);
}
"""

# FSR 1 RCAS on the EASU output, no denoise. The input is read with texelFetch from a sampled image (no sampler).
FSR_RCAS_COMP = "#version 450\n" + "#extension GL_EXT_samplerless_texture_functions : require\n" + FSR_COMMON_DEFINES + r"""#include "ffx_core.h"
#define FSR_RCAS_F 1

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 0) uniform texture2D u_src;                       // the EASU output, read with texelFetch
layout(set = 0, binding = 1, rgba16f) writeonly uniform image2D u_dst;
layout(push_constant) uniform PushConstants {
    uvec4 con;    // fsr1_rcas_consts() output
    vec4 opt;     // x: 1 = scene-linear HDR (compress/expand), 0 = [0,1]; y, z: output width, height; w: pre-scale k (0 = 1)
} pc;

bool linear_in() { return pc.opt.x > 0.5; }

float prescale() { return pc.opt.w > 0.0 ? pc.opt.w : 1.0; }

vec4 compress4(vec4 x) {
    x = max(x, vec4(0.0)) * prescale();
    return x / (vec4(1.0) + x);
}

vec3 expand3(vec3 c) {
    c = clamp(c, vec3(0.0), vec3(0.999));
    return c / (vec3(1.0) - c) / prescale();
}

// Input callbacks of the SDK's RCAS: reads are clamped to the output, so the 3x3 neighbourhood stays inside the image.
FfxFloat32x4 FsrRcasLoadF(FfxInt32x2 p) {
    vec4 t = texelFetch(u_src, clamp(p, ivec2(0), ivec2(pc.opt.yz) - 1), 0);
    return linear_in() ? compress4(t) : t;
}
void FsrRcasInputF(inout FfxFloat32 r, inout FfxFloat32 g, inout FfxFloat32 b) {}

#include "fsr1/ffx_fsr1.h"

void CurrFilter(uvec2 pos) {
    if (any(greaterThanEqual(pos, uvec2(pc.opt.yz)))) return;   // outside the output
    vec3 c;
    FsrRcasF(c.r, c.g, c.b, pos, pc.con);
    if (linear_in()) c = expand3(c);
    imageStore(u_dst, ivec2(pos), vec4(c, 1.0));
}

void main() {
    uvec2 gxy = ffxRemapForQuad(gl_LocalInvocationID.x) + uvec2(gl_WorkGroupID.x << 4u, gl_WorkGroupID.y << 4u);
    CurrFilter(gxy);
    gxy.x += 8u;
    CurrFilter(gxy);
    gxy.y += 8u;
    CurrFilter(gxy);
    gxy.x -= 8u;
    CurrFilter(gxy);
}
"""

# (C name, source file name, GLSL text, stage note for the header comment)
SHADERS = [
    ("kOutVert", "out_encode.vert", OUT_VERT),
    ("kOutEncodeFrag", "out_encode.frag", OUT_ENCODE_FRAG),
    ("kOverlayCompositeFrag", "overlay_composite.frag", OVERLAY_COMPOSITE_FRAG),
    ("kFsrEasuComp", "fsr_easu.comp", FSR_EASU_COMP),
    ("kFsrRcasComp", "fsr_rcas.comp", FSR_RCAS_COMP),
]

HEADER = """// GENERATED by scripts/gen_extras_shaders.py: do not edit
// SPIR-V for the output pass, compiled with glslc -O --target-env=vulkan1.3 from the GLSL in scripts/gen_extras_shaders.py and
// checked with spirv-val --target-env vulkan1.3. FSR 1 headers: third_party/ffx_fsr1 (AMD FidelityFX SDK v1.1.4, MIT).
// All shaders use descriptor set 0. Push constants are std430 (vec4/uvec4 blocks, 16-byte aligned; offsets are in bytes).
//
// kOutVert: vertex shader, full-screen triangle, drawn with 3 vertices and no vertex input. No descriptors, no push
//   constants. Output location 0: vec2 v_dst (destination position in [0,1]^2, row 0 at the top). Used with kOutEncodeFrag and
//   kOverlayCompositeFrag.
//
// kOutEncodeFrag: fragment shader for a graphics pipeline that writes the swapchain (or the frame image).
//   Descriptors: set 0, binding 0, COMBINED_IMAGE_SAMPLER (sampler2D u_src), fragment stage. The source is a linear-filtered
//     game picture; the same sampler setup as the present pass.
//     Bindings 1 (u_scene, the window texture before the UI) and 2 (u_final, the window texture as presented), same size, read at scn:
//     where they differ the UI covers the pixel and the highlight expansion is left out. Bind u_final to both when no copy exists.
//   Push constants: 128 bytes, VK_SHADER_STAGE_FRAGMENT_BIT.
//     offset 0   vec4 pic   picture rectangle in destination UV: x, y, w, h (0, 0, 1, 1 when the picture fills the target)
//     offset 16  vec4 crop  source crop in texels: x, y, w, h
//     offset 32  vec4 src   x, y: whole source size in texels; z: 1 = flip the picture vertically, 0 = no flip
//     offset 48  vec4 p     x: mode, read as int(x + 0.5):
//                             0 = copy, the texel unchanged (as present.frag)
//                             1 = scene-linear to sRGB (IEC 61966-2-1 OETF of clamp(rgb, 0, 1)), for an 8-bit UNORM swapchain
//                             2 = scene-linear to HDR10: 1.0 = paper white, BT.709 to BT.2020, nits = min(rgb * y, z) per
//                                 channel (negatives to 0), ST 2084 PQ signal, for an A2B10G10R10 swapchain
//                             3 = scene-linear to scRGB: min(rgb * y, z) / 80 (z = peak nits), for a float16 swapchain with
//                                 VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT
//                             any other value = copy
//                           y: paper white in nits (used by modes 2 and 3); z: peak luminance in nits (mode 2 limit);
//                           w: 1 = apply the picture controls of adj, 0 = the plain encode above
//     offset 64  vec4 adj   picture controls, x contrast, y saturation, z black level b, w gamma g, on the display-encoded signal c
//                           (modes 0 and 1: the output signal, clamped to [0,1] after; modes 2 and 3: rgb^(1/2.2) before the encode,
//                           ^2.2 after): c = b < 0 ? (c + b) / (1 + b) : b + c (1 - b); c = (c - 0.5) x + 0.5;
//                           c = l + (c - l) y with l the Rec. 709 luma of c; c = max(c, 0)^(1/g)
//     offset 80  vec4 scn   the picture in u_scene / u_final UV: origin xy, size zw
//     offset 96  vec4 ui    x: the UI's brightness over paper white in modes 2 and 3 (1 = paper white), where u_scene and u_final differ
//                           y: 1 = deband in modes 2 and 3: triangular noise of one step on the sRGB code of the source, none at 0 or 1
//                           z: test pattern in modes 2 and 3 (extras/hdr_pattern.h): 0 none, 1 peak (small tiles of 10000 nits around w nits, on black,
//                              PQ or scRGB with no peak limit), 2 black (tiles of 8-bit codes 1..16 on black, then the picture controls and the encode),
//                              3 staircase (8 bars of 300..1000 nits along the top 7%, on black, no peak limit)
//     offset 112 vec4 hx    x: 1 = the source (and u_scene / u_final) are float with display-encoded values, highlights above 1: decoded by sRGB up
//                           to 1 and a 2.2 power above; in modes 2 and 3 the largest channel m above 1 is then fitted under y, one factor per
//                           pixel, before the UI brightness: 1 + (y - 1)(1 - exp(-(m - 1) / (y - 1))); w: 1 = the source is clipped at 1 and decoded by sRGB instead
//                           (the 8-bit picture), no reshape (src.w's expansion applies as for an 8-bit source)
//   Output: location 0, vec4, alpha always 1. Letterbox bars are (0, 0, 0, 1) in every mode.
//
// kOverlayCompositeFrag: fragment shader that draws the ImGui overlay on an HDR swapchain image. Used with kOutVert.
//   The overlay was rendered into an RGBA8 UNORM image (cleared to 0), so its texels are premultiplied: (rgb * a, a).
//   Descriptors: set 0, binding 0, COMBINED_IMAGE_SAMPLER (sampler2D u_ui), fragment stage. The image has the swapchain's size
//     and is read at v_dst with a linear sampler, clamp to edge (the texel centres, so no scaling).
//   Push constants: 16 bytes, VK_SHADER_STAGE_FRAGMENT_BIT.
//     offset 0   vec4 p     x: mode, read as int(x + 0.5): 1 = SDR (the straight sRGB-encoded colour, for an SDR swapchain after the HDR / SDR
//                             compare switched to it), 2 = HDR10 (BT.709 to BT.2020, nits = min(rgb * y, z), ST 2084 PQ),
//                             3 = scRGB (rgb * y / 80, no clamp), any other value = scene-linear, unencoded.
//                           y: paper white nits (UI white, the sRGB value 1.0, maps to this); z: peak nits (mode 2 limit); w: unused.
//   Output: location 0, vec4 (encoded rgb of the un-premultiplied colour, a = the overlay's alpha). Texels with alpha 0 output
//     (0, 0, 0, 0). Blended with SRC_ALPHA / ONE_MINUS_SRC_ALPHA on the swapchain.
//
// kFsrEasuComp: compute shader, FSR 1 EASU (FP32 path). Local size 64 x 1 x 1. Each 16 x 16 output tile is one workgroup:
//   the 64 invocations each write four output pixels. Dispatch: ceil(out_w / 16) x ceil(out_h / 16) x 1. Pixels at or past
//   out_w / out_h are skipped inside the shader, so the dispatch may overshoot.
//   Descriptors: set 0, binding 0, COMBINED_IMAGE_SAMPLER (sampler2D u_src), compute stage. Use clamp-to-edge addressing.
//     textureGather returns the four texels around each point, so the filter setting does not affect the result.
//   set 0, binding 1, STORAGE_IMAGE (image2D u_dst, rgba16f, writeonly), compute stage. The output, out_w x out_h.
//   Push constants: 80 bytes, VK_SHADER_STAGE_COMPUTE_BIT.
//     offset 0   uvec4 con0   } the four words of fsr1_easu_consts() (src/gfx/extras/fsr1_consts.h), in this order
//     offset 16  uvec4 con1   }
//     offset 32  uvec4 con2   }
//     offset 48  uvec4 con3   }
//     offset 64  vec4 opt     x: perceptual flag. 0 = input is an sRGB-style [0,1] picture, used as is.
//                               1 = input is scene-linear HDR (1.0 = paper white, may exceed 1): each fetched texel is
//                               compressed c = x / (1 + x) (x clamped to >= 0) before EASU, and each result is expanded
//                               x = c / (1 - c) (c clamped to [0, 0.999]) after it. The output image then holds linear HDR.
//                             y: output width in pixels; z: output height in pixels; w: pre-scale k of the compression (0 = 1).
//
// kFsrRcasComp: compute shader, FSR 1 RCAS (FP32, sharpening without denoise). Local size 64 x 1 x 1, same tile and dispatch
//   as EASU. It reads the EASU output (the intermediate image, out_w x out_h) and writes the final image.
//   Descriptors: set 0, binding 0, SAMPLED_IMAGE (texture2D u_src), compute stage. Read with texelFetch, so no sampler is
//     bound for it. Reads outside the image are clamped to the edge.
//   set 0, binding 1, STORAGE_IMAGE (image2D u_dst, rgba16f, writeonly), compute stage.
//   Push constants: 32 bytes, VK_SHADER_STAGE_COMPUTE_BIT.
//     offset 0   uvec4 con  the four words of fsr1_rcas_consts() (src/gfx/extras/fsr1_consts.h)
//     offset 16  vec4 opt   x: perceptual flag, same meaning as EASU's (1 = scene-linear: compress before RCAS, expand after);
//                           y: output width; z: output height; w: pre-scale k (0 = 1).
"""


def hex_words(words, name):
    lines = ["static const uint32_t %s[] = {\n" % name]
    for i in range(0, len(words), 8):
        lines.append("    " + ", ".join("0x%08x" % w for w in words[i:i + 8]) + ",\n")
    lines.append("};\n")
    return "".join(lines)


def compile_glsl(src_name, text, tmp):
    src = os.path.join(tmp, src_name)
    with open(src, "w", newline="\n") as f:
        f.write(text)
    res = subprocess.run([glslc, "-O", "--target-env=vulkan1.3", "-I", fsr_inc, "-o", "-", src], capture_output=True)
    if res.returncode != 0:
        sys.stderr.write("glslc failed on %s:\n%s" % (src_name, res.stderr.decode(errors="replace")))
        sys.exit(1)
    if res.stderr:
        sys.stderr.write("glslc warnings on %s:\n%s" % (src_name, res.stderr.decode(errors="replace")))
    return res.stdout


def validate(spv, name, tmp):
    path = os.path.join(tmp, name + ".spv")
    with open(path, "wb") as f:
        f.write(spv)
    res = subprocess.run([spirv_val, "--target-env", "vulkan1.3", path], capture_output=True, text=True)
    return res.returncode == 0, (res.stdout + res.stderr).strip()


def main():
    body = [HEADER]
    rows = []
    failed = False
    with tempfile.TemporaryDirectory() as tmp:
        for cname, src_name, text in SHADERS:
            spv = compile_glsl(src_name, text, tmp)
            if len(spv) % 4:
                sys.stderr.write("%s: SPIR-V size is not a multiple of 4\n" % cname)
                sys.exit(1)
            ok, msg = validate(spv, cname, tmp)
            words = to_words(spv)
            rows.append((cname, len(words), "PASS" if ok else "FAIL"))
            if not ok:
                failed = True
                sys.stderr.write("spirv-val failed on %s:\n%s\n" % (cname, msg))
            body.append("\n" + hex_words(words, cname))
    print("%-16s %10s  %s" % ("name", "words", "spirv-val"))
    for name, n, verdict in rows:
        print("%-16s %10d  %s" % (name, n, verdict))
    if failed:
        print("FAILED: extras_shaders.inc not written")
        sys.exit(1)
    with open(out_path, "w", newline="\n") as f:
        f.write("".join(body))
    print("wrote %s" % out_path)


def to_words(spv):
    return struct.unpack("<%dI" % (len(spv) // 4), spv)


if __name__ == "__main__":
    main()
