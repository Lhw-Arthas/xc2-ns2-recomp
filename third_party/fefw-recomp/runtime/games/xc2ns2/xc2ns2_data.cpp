// SPDX-License-Identifier: GPL-3.0-or-later
// Xenoblade Chronicles 2 - Switch 2 Edition: identity and file names (nxrt/game.h Info). Pure data, no runtime.
//
// Facts: title 0100E95004038000, NS2 Edition (NUP v1, title version 0); ExeFS modules main/sdk/subsdk0/rtld, all NSO0;
// main 9.05 MB, .text 0x9bd3d0, build id 5087AF2917D85858A0FD49CB4D6EED67; sdk carries NintendoSDK_NVN-3_5_0-Release.
#include "xc2ns2_game.h"

namespace nxrt::games::xc2ns2 {

const game::Info& data_info() {
    static const game::Info info = [] {
        game::Info g;
        g.id = "xc2ns2";
        g.title = "Xenoblade Chronicles 2 (Switch 2 Edition)";
        g.product = "Xenoblade Chronicles 2 NS2 PC";
        g.short_title = "Xenoblade 2";
        g.program_id = 0x0100E95004038000ull;
        g.version = "0";                       // the NS2 Edition NUP reports title version 0
        g.main_base = 0x7100000000ull;
        // control.nacp is not in the dump yet: the NS2 build keeps every language of the original
        // (JPja USen EUen EUfr EUde EUit EUes KRko CNzh TWzh).
        g.languages = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 7) | (1u << 12) | (1u << 13) |
                      (1u << 14) | (1u << 15) | (1u << 16);
        g.platform = game::Platform::Switch2;
        // TODO: the exact API minor of NintendoSDK_NVN-3_5_0 (the base reads it from nvnDeviceGetInteger 0/1). 30 is Pokopia's
        // (NVN 20.5.25); XC2 is older, so this is a placeholder until the sdk's nvnDeviceGetInteger is disassembled or the census
        // logs the value the game asks for.
        g.nvn_api_minor = 30;
        g.present_interval_hz = 0;             // XC2 paces itself; set once the boot log shows its vsync handling
        g.frame_gen_hudless_default = true;    // XC2 draws its HUD directly (blended) onto the window texture after DLSS eval (#53)
        g.exe_name = "xc2ns2.exe";
        g.setup_exe_name = "xc2ns2_setup.exe";
        g.launcher_ini = "xc2ns2_launcher.ini";
        g.game_ini = "xc2ns2.ini";
        g.shader_pack = "shader_cache.xc2ns2shc";
        g.save_folder = "Xenoblade2NS2RecompPC";
        g.jc2_mouse_default = "";              // auto: XC2 has no pointer UI
        return g;
    }();
    return info;
}

}  // namespace nxrt::games::xc2ns2

void nxrt_register_game_xc2ns2_data() { nxrt::game::set_current(&nxrt::games::xc2ns2::data_info()); }
