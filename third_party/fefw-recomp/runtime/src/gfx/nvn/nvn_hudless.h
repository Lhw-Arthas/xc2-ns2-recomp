// SPDX-License-Identifier: GPL-3.0-or-later
// The scene without the UI: the game draws its UI into the window texture after the scene, so the window texture just before the
// first blended draw into it is the scene alone. It is copied there for DLSS Frame Generation (its HUD-less colour, nvn_framegen.h)
// and HDR output (the UI kept out of the highlight boost, nvn_hdr_ui.h).
#pragma once
#include "nvn_gpu_internal.h"

namespace nxrt::nvn::be::gpu {

// A colour image of one level and layer whose layout is tracked in recording order.
struct TrackedImage {
    gfx::Image img;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;

    // Remakes the image when the size or format changed (the old one is destroyed once the GPU is done). False when it cannot be made.
    bool ensure(VkExtent2D e, VkFormat fmt, VkImageUsageFlags usage, const char* name);
    void to(VkCommandBuffer cb, VkImageLayout l);
};

VkFormat hudless_copy_format(VkFormat tex_fmt, bool scene_hdr);   // copy image format matching present's sRGB/UNORM view choice

// Who wants a copy: frame generation for the current frame (after its DLSS evaluation), HDR for every frame while it needs one.
// The first ask of a frame drops a copy made before it, so frame generation always gets the first blended draw after its evaluation.
void hudless_want_this_frame();
void hudless_want_every_frame(bool on);

// replay, before a draw into colour target t (blended: blending on target 0): true = call hudless_capture now, with no pass open
bool hudless_candidate(Texture* t, bool blended);
void hudless_capture(Frame& f, Texture* t);
bool hudless_failed_this_frame();     // a capture of this frame failed (no copy, and no later draw of the frame is tried)
uint64_t hudless_capture_count();     // copies made so far

// present: this frame's copy of `presented` in SHADER_READ_ONLY_OPTIMAL, or null when there is none. A repeated present (again)
// gets the copy of the frame it repeats.
VkImageView hudless_view(Texture* presented, Frame& f, bool again);
// present, after every user: the window's textures for the next captures; the next frame's copy goes to the other image
void hudless_end_present(Window* w, bool again);
void hudless_shutdown();   // frees the copies and forgets everything (render thread, GPU idle)

}  // namespace nxrt::nvn::be::gpu
