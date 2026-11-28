// SPDX-License-Identifier: GPL-3.0-or-later
// NVDEC (nvcuvid) decoding: see video_nvdec.h. One parser per decoder, fed one temporal unit at a time; the pictures the
// parser shows are copied out as NV12, which is what the movie library's pool wants.
#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

#include "../../third_party/ffnvcodec/dynlink_loader.h"
#include "nxrt/log.h"
#include "video_nvdec.h"

namespace nxrt::media {
namespace {

constexpr const char* kTag = "nvdec";

cudaVideoCodec to_cuvid(int codec) {
    switch (codec) {
        case 9: return cudaVideoCodec_VP9;
        case 8: return cudaVideoCodec_VP8;
        default: return cudaVideoCodec_AV1;
    }
}

}  // namespace

struct NvDec {
    CudaFunctions* cu = nullptr;
    CuvidFunctions* cv = nullptr;
    CUcontext ctx = nullptr;
    CUvideoparser parser = nullptr;
    CUvideodecoder dec = nullptr;
    int codec = 10;
    int w = 0, h = 0;
    bool failed = false;
    bool primary = false;
    std::vector<NvDecPicture>* out = nullptr;
};

namespace {

int CUDAAPI on_sequence(void* ud, CUVIDEOFORMAT* fmt) {
    NvDec* d = static_cast<NvDec*>(ud);
    if (!d || fmt->codec != to_cuvid(d->codec)) { if (d) d->failed = true; return 0; }
    d->w = static_cast<int>(fmt->coded_width);
    d->h = static_cast<int>(fmt->coded_height);
    if (d->w <= 0 || d->h <= 0) { d->failed = true; return 0; }
    CUVIDDECODECREATEINFO ci = {};
    ci.ulWidth = ci.ulMaxWidth = ci.ulTargetWidth = d->w;
    ci.ulHeight = ci.ulMaxHeight = ci.ulTargetHeight = d->h;
    ci.ulNumDecodeSurfaces = d->codec == 10 ? 16 : 8;
    ci.ulNumOutputSurfaces = 2;
    ci.CodecType = to_cuvid(d->codec);
    ci.ChromaFormat = cudaVideoChromaFormat_420;
    ci.ulCreationFlags = cudaVideoCreate_PreferCUVID;
    ci.bitDepthMinus8 = fmt->bit_depth_luma_minus8;
    ci.OutputFormat = cudaVideoSurfaceFormat_NV12;
    ci.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave;
    CUresult r = d->cv->cuvidCreateDecoder(&d->dec, &ci);
    NXLOG_I(kTag, "cuvidCreateDecoder %dx%d -> %d", d->w, d->h, static_cast<int>(r));
    if (r != CUDA_SUCCESS) { d->failed = true; return 0; }
    NXLOG_I(kTag, "NVDEC decoder: %dx%d, %d-bit, %d surfaces", d->w, d->h, 8 + ci.bitDepthMinus8, ci.ulNumDecodeSurfaces);
    return 1;
}

int CUDAAPI on_decode(void* ud, CUVIDPICPARAMS* p) {
    NvDec* d = static_cast<NvDec*>(ud);
    if (!d || !d->dec) return 0;
    return d->cv->cuvidDecodePicture(d->dec, p) == CUDA_SUCCESS ? 1 : 0;
}

int CUDAAPI on_display(void* ud, CUVIDPARSERDISPINFO* info) {
    NvDec* d = static_cast<NvDec*>(ud);
    if (!d || !d->dec || !d->out) return 1;
    CUVIDPROCPARAMS pp = {};
    pp.progressive_frame = 1;
    pp.top_field_first = 1;
    unsigned int pitch = 0;
    CUdeviceptr dev = 0;
    if (d->cv->cuvidMapVideoFrame(d->dec, info->picture_index, &dev, &pitch, &pp) != CUDA_SUCCESS) return 1;
    NvDecPicture pic;
    pic.w = d->w;
    pic.h = d->h;
    pic.pts = info->timestamp;
    pic.nv12.resize(static_cast<size_t>(d->w) * d->h * 3 / 2);
    // The mapped frame is device memory: copy both NV12 planes to the host with the driver (a plain memcpy faults).
    const bool pushed = d->cu->cuCtxPushCurrent(d->ctx) == CUDA_SUCCESS;
    bool ok = pushed;
    for (int plane = 0; ok && plane < 2; ++plane) {
        CUDA_MEMCPY2D c = {};
        c.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        c.srcDevice = dev + (plane ? static_cast<CUdeviceptr>(pitch) * d->h : 0);
        c.srcPitch = pitch;
        c.dstMemoryType = CU_MEMORYTYPE_HOST;
        c.dstHost = pic.nv12.data() + (plane ? static_cast<size_t>(d->w) * d->h : 0);
        c.dstPitch = static_cast<size_t>(d->w);
        c.WidthInBytes = static_cast<size_t>(d->w);
        c.Height = static_cast<size_t>(plane ? d->h / 2 : d->h);
        ok = d->cu->cuMemcpy2D(&c) == CUDA_SUCCESS;
    }
    CUcontext popped = nullptr;
    if (pushed) d->cu->cuCtxPopCurrent(&popped);
    d->cv->cuvidUnmapVideoFrame(d->dec, dev);
    if (ok) d->out->push_back(std::move(pic));
    return 1;
}

}  // namespace

bool nvdec_available(std::string* why) {
    CudaFunctions* cu = nullptr;
    CuvidFunctions* cv = nullptr;
    if (cuda_load_functions(&cu, nullptr) < 0) { if (why) *why = "nvcuda.dll did not load"; return false; }
    if (cuvid_load_functions(&cv, nullptr) < 0) { cuda_free_functions(&cu); if (why) *why = "nvcuvid.dll did not load"; return false; }
    int devices = 0;
    const bool ok = cu->cuInit(0) == CUDA_SUCCESS && cu->cuDeviceGetCount(&devices) == CUDA_SUCCESS && devices > 0;
    if (!ok && why) *why = "no CUDA device";
    cuvid_free_functions(&cv);
    cuda_free_functions(&cu);
    return ok;
}

NvDec* nvdec_create(int codec, int max_w, int max_h, std::string* why) {
    (void)max_w; (void)max_h;
    auto* d = new NvDec();
    d->codec = codec;
    if (cuda_load_functions(&d->cu, nullptr) < 0) { if (why) *why = "nvcuda.dll did not load"; delete d; return nullptr; }
    if (cuvid_load_functions(&d->cv, nullptr) < 0) { cuda_free_functions(&d->cu); if (why) *why = "nvcuvid.dll did not load"; delete d; return nullptr; }
    CUdevice dev = 0;
    // The device's primary context, not one of our own: the parser runs its callbacks on whichever thread calls
    // cuvidParseVideoData, and a primary context can be current on several threads, an ordinary one cannot.
    if (d->cu->cuInit(0) != CUDA_SUCCESS || d->cu->cuDeviceGet(&dev, 0) != CUDA_SUCCESS ||
        d->cu->cuDevicePrimaryCtxRetain(&d->ctx, dev) != CUDA_SUCCESS) {
        if (why) *why = "no CUDA context";
        cuvid_free_functions(&d->cv); cuda_free_functions(&d->cu); delete d; return nullptr;
    }
    d->primary = true;
    CUVIDPARSERPARAMS pp = {};
    pp.CodecType = to_cuvid(codec);
    pp.ulMaxNumDecodeSurfaces = 16;
    pp.ulClockRate = 10000000;
    pp.ulMaxDisplayDelay = 0;
    pp.pUserData = d;
    pp.pfnSequenceCallback = on_sequence;
    pp.pfnDecodePicture = on_decode;
    pp.pfnDisplayPicture = on_display;
    NXLOG_I(kTag, "nvcuvid entry points: parser %d decoder %d decode %d map %d unmap %d caps %d",
            d->cv->cuvidCreateVideoParser != nullptr, d->cv->cuvidCreateDecoder != nullptr,
            d->cv->cuvidDecodePicture != nullptr, d->cv->cuvidMapVideoFrame != nullptr,
            d->cv->cuvidUnmapVideoFrame != nullptr, d->cv->cuvidGetDecoderCaps != nullptr);
    if (!d->cv->cuvidCreateVideoParser || !d->cv->cuvidCreateDecoder || !d->cv->cuvidDecodePicture ||
        !d->cv->cuvidMapVideoFrame || !d->cv->cuvidUnmapVideoFrame) {
        if (why) *why = "nvcuvid.dll lacks an entry point this backend uses";
        nvdec_destroy(d);
        return nullptr;
    }
    d->cu->cuCtxPushCurrent(d->ctx);
    const CUresult r = d->cv->cuvidCreateVideoParser(&d->parser, &pp);
    NXLOG_I(kTag, "cuvidCreateVideoParser -> %d", static_cast<int>(r));
    d->cu->cuCtxPopCurrent(nullptr);
    if (r != CUDA_SUCCESS) {
        if (why) *why = "cuvidCreateVideoParser failed";
        nvdec_destroy(d);
        return nullptr;
    }
    return d;
}

bool nvdec_decode(NvDec* d, const uint8_t* data, size_t size, int64_t pts, std::vector<NvDecPicture>* out) {
    if (!d || d->failed || !d->parser || !out) return false;
    d->out = out;
    d->cu->cuCtxPushCurrent(d->ctx);
    CUVIDSOURCEDATAPACKET pkt = {};
    pkt.payload = data;
    pkt.payload_size = static_cast<unsigned long>(size);
    pkt.flags = CUVID_PKT_TIMESTAMP;
    pkt.timestamp = pts;
    const CUresult r = d->cv->cuvidParseVideoData(d->parser, &pkt);
    d->cu->cuCtxPopCurrent(nullptr);
    d->out = nullptr;
    if (r != CUDA_SUCCESS || d->failed) {
        NXLOG_W(kTag, "cuvidParseVideoData failed: %d (failed flag %d)", static_cast<int>(r), d->failed ? 1 : 0);
        d->failed = true;
        return false;
    }
    return true;
}

void nvdec_destroy(NvDec* d) {
    if (!d) return;
    if (d->cu && d->ctx) d->cu->cuCtxPushCurrent(d->ctx);
    if (d->cv && d->parser) {
        CUVIDSOURCEDATAPACKET eos = {};
        eos.flags = CUVID_PKT_ENDOFSTREAM;
        d->cv->cuvidParseVideoData(d->parser, &eos);
        d->cv->cuvidDestroyVideoParser(d->parser);
    }
    if (d->cv && d->dec) d->cv->cuvidDestroyDecoder(d->dec);
    if (d->cu && d->ctx) {
        d->cu->cuCtxPopCurrent(nullptr);
        CUdevice dev = 0;
        if (d->primary && d->cu->cuDeviceGet(&dev, 0) == CUDA_SUCCESS) d->cu->cuDevicePrimaryCtxRelease(dev);
        else d->cu->cuCtxDestroy(d->ctx);
    }
    if (d->cv) cuvid_free_functions(&d->cv);
    if (d->cu) cuda_free_functions(&d->cu);
    delete d;
}

}  // namespace nxrt::media
