// SPDX-License-Identifier: GPL-3.0-or-later
// HDR patch of XC2's tone map: the highlights from the scene (hdr_tonemap_patch.h, spirv_hdr_tonemap_scene).
#include <cstring>
#include <unordered_map>
#include <vector>

#include "extras/hdr_tonemap_patch.h"

namespace nxrt::extras {
namespace {

enum : uint32_t {
    kOpExtInstImport = 11, kOpExtInst = 12, kOpEntryPoint = 15, kOpTypeInt = 21, kOpTypeFloat = 22, kOpTypeVector = 23, kOpTypeImage = 25,
    kOpTypeSampledImage = 27, kOpTypePointer = 32, kOpConstant = 43, kOpFunction = 54, kOpFunctionEnd = 56, kOpVariable = 59, kOpLoad = 61,
    kOpStore = 62, kOpAccessChain = 65, kOpDecorate = 71, kOpCompositeConstruct = 80, kOpImageSampleImplicitLod = 87, kOpFSub = 131,
    kOpFMul = 133, kOpFDiv = 136, kOpCompositeExtract = 81, kOpLabel = 248, kOpReturn = 253,
    kGlslPow = 26, kGlslExp2 = 29, kGlslLog2 = 30, kGlslFMax = 40, kGlslFClamp = 43, kGlslFma = 50,
    kExecFragment = 4, kDim3D = 2, kDecLocation = 30, kScOutput = 3, kScPrivate = 6,
};

struct Inst {
    size_t pos;
    uint32_t op, len;
};

constexpr float kKnee = 0.25f;   // the scene value (1 = the game's white) where the HDR curve leaves the game's: about mid grey, where its shoulder starts

uint32_t float_bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

}  // namespace

bool spirv_hdr_tonemap_scene(const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
    if (in.size() < 8 || in[0] != 0x07230203u) return false;
    std::vector<Inst> insts;
    std::unordered_map<uint32_t, size_t> def;   // result id -> instruction (the kinds the match follows)
    for (size_t i = 5; i < in.size();) {
        const uint32_t op = in[i] & 0xffff, len = in[i] >> 16;
        if (len == 0 || i + len > in.size()) return false;
        insts.push_back({i, op, len});
        if ((op == kOpTypeImage || op == kOpTypeSampledImage) && len >= 2) def[in[i + 1]] = insts.size() - 1;
        if ((op == kOpLoad || op == kOpExtInst || op == kOpCompositeConstruct || op == kOpFMul) && len >= 3) def[in[i + 2]] = insts.size() - 1;
        i += len;
    }
    auto get = [&](uint32_t id) -> const Inst* {
        auto it = def.find(id);
        return it == def.end() ? nullptr : &insts[it->second];
    };

    uint32_t glsl = 0, entry = 0, t_float = 0, t_int = 0, t_v4 = 0, c0 = 0, c1 = 0;
    size_t entry_inst = SIZE_MAX;
    for (size_t k = 0; k < insts.size(); ++k) {
        const uint32_t* w = &in[insts[k].pos];
        if (insts[k].op == kOpEntryPoint && insts[k].len >= 3 && w[1] == kExecFragment) { entry = w[2]; entry_inst = k; }
        if (insts[k].op == kOpExtInstImport && insts[k].len >= 5 && std::memcmp(&w[2], "GLSL.std.450", 12) == 0) glsl = w[1];
        if (insts[k].op == kOpTypeFloat && insts[k].len == 3 && w[2] == 32) t_float = w[1];
    }
    if (!entry || !glsl || !t_float) return false;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.pos];
        if (n.op == kOpTypeInt && n.len == 4 && w[2] == 32 && w[3] == 1) t_int = w[1];
        if (n.op == kOpTypeVector && n.len == 4 && w[2] == t_float && w[3] == 4) t_v4 = w[1];
        if (n.op == kOpConstant && n.len == 4 && w[1] == t_float && w[3] == float_bits(0.0f) && !c0) c0 = w[2];
        if (n.op == kOpConstant && n.len == 4 && w[1] == t_float && w[3] == float_bits(1.0f) && !c1) c1 = w[2];
    }
    if (!t_int || !t_v4 || !c0 || !c1) return false;

    size_t fn_begin = SIZE_MAX, fn_end = SIZE_MAX;
    std::unordered_map<uint32_t, uint32_t> stored;   // Function variable -> the one value stored to it (0: stored more than once)
    for (size_t k = 0; k < insts.size(); ++k) {
        const Inst& n = insts[k];
        const uint32_t* w = &in[n.pos];
        if (n.op == kOpFunction && n.len >= 3 && w[2] == entry) fn_begin = k;
        if (n.op == kOpFunctionEnd && fn_begin != SIZE_MAX && fn_end == SIZE_MAX) fn_end = k;
        if (n.op == kOpStore && n.len == 3) {
            auto [it, fresh] = stored.try_emplace(w[1], w[2]);
            if (!fresh) it->second = 0;
        }
    }
    if (fn_begin == SIZE_MAX || fn_end == SIZE_MAX) return false;
    // through a Function variable written once: the value stored
    auto resolve = [&](uint32_t id) {
        for (int i = 0; i < 4; ++i) {
            const Inst* d = get(id);
            if (!d || d->op != kOpLoad) break;
            auto it = stored.find(in[d->pos + 3]);
            if (it == stored.end() || !it->second) break;
            id = it->second;
        }
        return id;
    };
    auto glsl_op = [&](uint32_t id, uint32_t fn, uint32_t len) -> const Inst* {
        const Inst* d = get(resolve(id));
        return d && d->op == kOpExtInst && d->len == len && in[d->pos + 3] == glsl && in[d->pos + 4] == fn ? d : nullptr;
    };

    // two coordinates built from the same three values (each read back through its variable)
    auto same_coord = [&](uint32_t a, uint32_t b) {
        if (a == b) return true;
        const Inst* x = get(a);
        const Inst* y = get(b);
        if (!x || !y || x->op != kOpCompositeConstruct || y->op != kOpCompositeConstruct || x->len != y->len) return false;
        for (uint32_t i = 3; i < x->len; ++i)
            if (resolve(in[x->pos + i]) != resolve(in[y->pos + i])) return false;
        return true;
    };
    // one or two 3D samples (the grading volumes; two are blended by depth) at one coordinate: three Fma(Fma(sel, d, shaped), scale, bias)
    int n3d = 0;
    uint32_t coord = 0, lut = 0, smp_type = 0;
    for (const Inst& n : insts) {
        if (n.op != kOpImageSampleImplicitLod || n.len < 5) continue;
        const Inst* si = get(in[n.pos + 3]);
        const Inst* sty = si && si->op == kOpLoad ? get(in[si->pos + 1]) : nullptr;
        const Inst* img = sty && sty->op == kOpTypeSampledImage ? get(in[sty->pos + 2]) : nullptr;
        if (!img || img->op != kOpTypeImage || in[img->pos + 3] != kDim3D) continue;
        if (n3d++ == 0) {
            coord = in[n.pos + 4];
            smp_type = in[n.pos + 1];
            lut = in[n.pos + 3];
        } else if (!same_coord(in[n.pos + 4], coord)) {
            return false;
        }
    }
    if (n3d < 1 || n3d > 2) return false;
    const Inst* cc = get(coord);
    if (!cc || cc->op != kOpCompositeConstruct || cc->len != 6) return false;
    // each component back to the shaper: Exp2(Log2(FClamp(x * exposure, 0, 1)) * exponent), x the scene channel
    std::vector<size_t> clamps;
    uint32_t lut_scale = 0, lut_bias = 0, shaper_exp = 0;
    for (int c = 0; c < 3; ++c) {
        const Inst* f = glsl_op(in[cc->pos + 3 + c], kGlslFma, 8);
        const Inst* inner = f ? glsl_op(in[f->pos + 5], kGlslFma, 8) : nullptr;
        const Inst* ex = inner ? glsl_op(in[inner->pos + 7], kGlslExp2, 6) : nullptr;
        const Inst* ml = ex ? get(resolve(in[ex->pos + 5])) : nullptr;
        if (!ml || ml->op != kOpFMul || ml->len != 5) return false;
        const Inst* lg = glsl_op(in[ml->pos + 3], kGlslLog2, 6);
        const uint32_t e = lg ? in[ml->pos + 4] : in[ml->pos + 3];
        if (!lg) lg = glsl_op(in[ml->pos + 4], kGlslLog2, 6);
        const Inst* cl = lg ? glsl_op(in[lg->pos + 5], kGlslFClamp, 8) : nullptr;
        if (!cl || in[cl->pos + 6] != c0 || in[cl->pos + 7] != c1) return false;
        const Inst* x = get(resolve(in[cl->pos + 5]));
        if (!x || x->op != kOpFMul) return false;
        const size_t k = static_cast<size_t>(cl - insts.data());
        if (k < fn_begin || k > fn_end) return false;
        clamps.push_back(k);
        if (c == 0) { lut_scale = in[f->pos + 6]; lut_bias = in[f->pos + 7]; shaper_exp = e; }
    }
    if (clamps[0] == clamps[1] || clamps[1] == clamps[2] || clamps[0] == clamps[2]) return false;
    // one block: the values above are read again at the end of the entry function
    uint32_t t_v3 = 0;
    int labels = 0;
    for (size_t k = 0; k < insts.size(); ++k) {
        const uint32_t* w = &in[insts[k].pos];
        if (insts[k].op == kOpTypeVector && insts[k].len == 4 && w[2] == t_float && w[3] == 3) t_v3 = w[1];
        if (insts[k].op == kOpLabel && k > fn_begin && k < fn_end) ++labels;
    }
    if (labels != 1 || !t_v3) return false;

    // the output: an Output vec4 at Location 0, written per component through Output float pointers
    std::unordered_map<uint32_t, uint32_t> location, int_value;
    uint32_t p_out_v4 = 0, p_out_f = 0, out_var = 0;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.pos];
        if (n.op == kOpDecorate && n.len == 4 && w[2] == kDecLocation) location[w[1]] = w[3];
        if (n.op == kOpConstant && n.len == 4 && w[1] == t_int) int_value[w[2]] = w[3];
        if (n.op == kOpTypePointer && n.len == 4 && w[2] == kScOutput && w[3] == t_v4) p_out_v4 = w[1];
        if (n.op == kOpTypePointer && n.len == 4 && w[2] == kScOutput && w[3] == t_float) p_out_f = w[1];
    }
    if (!p_out_v4 || !p_out_f) return false;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.pos];
        if (n.op == kOpVariable && n.len >= 4 && w[1] == p_out_v4 && w[3] == kScOutput && location.count(w[2]) && location[w[2]] == 0) out_var = w[2];
    }
    if (!out_var) return false;
    uint32_t idx[3] = {0, 0, 0}, alpha_ptr = 0;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.pos];
        if (n.op != kOpAccessChain || n.len != 5 || w[1] != p_out_f || w[3] != out_var) continue;
        auto it = int_value.find(w[4]);
        if (it != int_value.end() && it->second < 3) idx[it->second] = w[4];
        if (it != int_value.end() && it->second == 3) alpha_ptr = w[2];
    }
    if (!idx[0] || !idx[1] || !idx[2] || !alpha_ptr) return false;
    // an opaque pass: alpha is written as 1.0 (a blended effect that grades through a LUT is left alone)
    bool alpha_one = false;
    for (const Inst& n : insts)
        if (n.op == kOpStore && n.len == 3 && in[n.pos + 1] == alpha_ptr) alpha_one = in[n.pos + 2] == c1;
    if (!alpha_one) return false;
    size_t first_fn = SIZE_MAX;
    for (size_t k = 0; k < insts.size() && first_fn == SIZE_MAX; ++k)
        if (insts[k].op == kOpFunction) first_fn = k;

    // new ids
    uint32_t bound = in[3];
    const uint32_t p_priv = bound++;
    const uint32_t var[3] = {bound++, bound++, bound++};
    const uint32_t cKnee = bound++, cKnee2 = bound++, cInvStep = bound++, cGamma = bound++, cInvGamma = bound++, cEps = bound++, cSharp = bound++;
    const struct { uint32_t id; float v; } consts[] = {{cKnee, kKnee}, {cKnee2, kKnee * 1.25f}, {cInvStep, 1.0f / (kKnee * 0.25f)}, {cGamma, 2.2f},
                                                       {cInvGamma, 1.0f / 2.2f}, {cEps, 1e-4f}, {cSharp, 1000.0f}};

    out.clear();
    out.reserve(in.size() + 160);
    out.insert(out.end(), in.begin(), in.begin() + 5);
    auto emit = [&](std::initializer_list<uint32_t> words, uint32_t op) {
        out.push_back((static_cast<uint32_t>(words.size() + 1) << 16) | op);
        out.insert(out.end(), words.begin(), words.end());
    };
    auto ext = [&](uint32_t res, uint32_t fn, std::initializer_list<uint32_t> args) {
        out.push_back((static_cast<uint32_t>(5 + args.size()) << 16) | kOpExtInst);
        out.push_back(t_float);
        out.push_back(res);
        out.push_back(glsl);
        out.push_back(fn);
        out.insert(out.end(), args.begin(), args.end());
    };
    const bool interface_all = in[1] >= 0x00010400u;   // SPIR-V 1.4+: every global the entry point uses is in its interface
    for (size_t k = 0; k < insts.size(); ++k) {
        const Inst& n = insts[k];
        const auto copy = [&] { out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(n.pos), in.begin() + static_cast<std::ptrdiff_t>(n.pos + n.len)); };
        if (k == entry_inst && interface_all) {
            out.push_back(((n.len + 3) << 16) | n.op);
            out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(n.pos + 1), in.begin() + static_cast<std::ptrdiff_t>(n.pos + n.len));
            out.insert(out.end(), {var[0], var[1], var[2]});
            continue;
        }
        if (k == first_fn) {   // globals: the Private float pointer, the constants, the three channel variables
            emit({p_priv, kScPrivate, t_float}, kOpTypePointer);
            for (const auto& c : consts) emit({t_float, c.id, float_bits(c.v)}, kOpConstant);
            for (uint32_t v : var) emit({p_priv, v, kScPrivate, c0}, kOpVariable);
        }
        if (n.op == kOpReturn && k > fn_begin && k < fn_end) {
            // sdr(v): the game's own response to a grey of scene value v (its shaper and LUT), display-linear
            auto sdr = [&](uint32_t v) {
                const uint32_t cl = bound++, lg = bound++, ml = bound++, ex = bound++, co = bound++, vec = bound++, smp = bound++, y = bound++, d = bound++;
                ext(cl, kGlslFClamp, {v, c0, c1});
                ext(lg, kGlslLog2, {cl});
                emit({t_float, ml, lg, shaper_exp}, kOpFMul);
                ext(ex, kGlslExp2, {ml});
                ext(co, kGlslFma, {ex, lut_scale, lut_bias});
                emit({t_v3, vec, co, co, co}, kOpCompositeConstruct);
                emit({smp_type, smp, lut, vec}, kOpImageSampleImplicitLod);
                emit({t_float, y, smp, 1}, kOpCompositeExtract);
                ext(d, kGlslPow, {y, cGamma});
                return d;
            };
            // m the largest scene channel (times the exposure; 1 = the game's white). Above the knee the game's curve flattens; the HDR curve goes
            // on at the game's slope at the knee instead: hdr = sdr(k) + s (m - k). The graded colour is scaled by hdr / sdr(m) (one factor,
            // display-encoded: to the power 1/2.2); below the knee the factor is 1.
            const uint32_t x0 = bound++, x1 = bound++, x2 = bound++, m01 = bound++, m = bound++;
            emit({t_float, x0, var[0]}, kOpLoad);
            emit({t_float, x1, var[1]}, kOpLoad);
            emit({t_float, x2, var[2]}, kOpLoad);
            ext(m01, kGlslFMax, {x0, x1});
            ext(m, kGlslFMax, {m01, x2});
            const uint32_t sm = sdr(m), sk = sdr(cKnee), sk2 = sdr(cKnee2);
            const uint32_t ds = bound++, sl = bound++, dm = bound++, lin = bound++, smx = bound++, ratio = bound++, rm1 = bound++, tw = bound++, t = bound++,
                           f0 = bound++, f = bound++, lg = bound++, lgs = bound++, sc = bound++;
            emit({t_float, ds, sk2, sk}, kOpFSub);
            emit({t_float, sl, ds, cInvStep}, kOpFMul);
            emit({t_float, dm, m, cKnee}, kOpFSub);
            ext(lin, kGlslFma, {sl, dm, sk});
            ext(smx, kGlslFMax, {sm, cEps});
            emit({t_float, ratio, lin, smx}, kOpFDiv);
            emit({t_float, rm1, ratio, c1}, kOpFSub);
            emit({t_float, tw, dm, cSharp}, kOpFMul);
            ext(t, kGlslFClamp, {tw, c0, c1});
            ext(f0, kGlslFma, {rm1, t, c1});
            ext(f, kGlslFMax, {f0, c1});
            ext(lg, kGlslLog2, {f});
            emit({t_float, lgs, lg, cInvGamma}, kOpFMul);
            ext(sc, kGlslExp2, {lgs});
            uint32_t ptr[3], col[3], res[3];
            for (int c = 0; c < 3; ++c) { ptr[c] = bound++; col[c] = bound++; res[c] = bound++; }
            for (int c = 0; c < 3; ++c) {
                emit({p_out_f, ptr[c], out_var, idx[c]}, kOpAccessChain);
                emit({t_float, col[c], ptr[c]}, kOpLoad);
                emit({t_float, res[c], col[c], sc}, kOpFMul);
                emit({ptr[c], res[c]}, kOpStore);
            }
            copy();
            continue;
        }
        copy();
        for (size_t c = 0; c < 3; ++c)   // after each clamp: keep its input, the scene channel times the exposure
            if (k == clamps[c]) emit({var[c], in[n.pos + 5]}, kOpStore);
    }
    out[3] = bound;
    return true;
}

}  // namespace nxrt::extras
