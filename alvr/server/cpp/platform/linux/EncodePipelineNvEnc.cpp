#include "EncodePipelineNvEnc.h"
#include "ALVR-common/packet_types.h"
#include "alvr_server/Logger.h"
#include "alvr_server/Settings.h"
#include "ffmpeg_helper.h"
#include <chrono>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace {

const char *encoder(ALVR_CODEC codec) {
    switch (codec) {
    case ALVR_CODEC_H264:
        return "h264_nvenc";
    case ALVR_CODEC_H265:
        return "hevc_nvenc";
    }
    throw std::runtime_error("invalid codec " + std::to_string(codec));
}

} // namespace
alvr::EncodePipelineNvEnc::EncodePipelineNvEnc(std::vector<VkFrame> &input_frames,
                                               VkFrameCtx &vk_frame_ctx) {
    int err;

    for (auto &input_frame : input_frames) {
        vk_frames.push_back(input_frame.make_av_frame(vk_frame_ctx).get());
    }

    err = AVUTIL.av_hwdevice_ctx_create(&hw_ctx, AV_HWDEVICE_TYPE_CUDA, NULL, NULL, 0);
    if (err < 0) {
        throw alvr::AvException("Failed to create a CUDA device:", err);
    }

    AVBufferRef *hw_frames_ref;
    auto input_frame_ctx = (AVHWFramesContext*)vk_frame_ctx.ctx->data;

    if (!(hw_frames_ref = AVUTIL.av_hwframe_ctx_alloc(hw_ctx))) {
        throw std::runtime_error("Failed to create CUDA frame context.");
    }
    auto frames_ctx = reinterpret_cast<AVHWFramesContext*>(hw_frames_ref->data);
    frames_ctx->format = AV_PIX_FMT_CUDA;
    frames_ctx->sw_format = AV_PIX_FMT_BGR0;
    frames_ctx->width = input_frame_ctx->width;
    frames_ctx->height = input_frame_ctx->height;
    if ((err = AVUTIL.av_hwframe_ctx_init(hw_frames_ref)) < 0) {
        AVUTIL.av_buffer_unref(&hw_frames_ref);
        throw alvr::AvException("Failed to initialize CUDA frame context:", err);
    }

    const auto &settings = Settings::Instance();

    auto codec_id = ALVR_CODEC(settings.m_codec);
    const char *encoder_name = encoder(codec_id);
    AVCodec *codec = AVCODEC.avcodec_find_encoder_by_name(encoder_name);
    if (codec == nullptr) {
        throw std::runtime_error(std::string("Failed to find encoder ") + encoder_name);
    }

    encoder_ctx = AVCODEC.avcodec_alloc_context3(codec);
    if (not encoder_ctx) {
        throw std::runtime_error("failed to allocate NVEnc encoder");
    }

    switch (codec_id) {
    case ALVR_CODEC_H264:
        AVUTIL.av_opt_set(encoder_ctx, "preset", "llhq", 0);
        AVUTIL.av_opt_set(encoder_ctx, "zerolatency", "1", 0);
        break;
    case ALVR_CODEC_H265:
        AVUTIL.av_opt_set(encoder_ctx, "preset", "llhq", 0);
        AVUTIL.av_opt_set(encoder_ctx, "zerolatency", "1", 0);
        break;
    }

    /**
     * We will recieve a frame from HW as AV_PIX_FMT_VULKAN which will converted to AV_PIX_FMT_BGRA
     * as SW format when we get it from HW.
     * But NVEnc support only BGR0 format and we easy can just to force it
     * Because:
     * AV_PIX_FMT_BGRA - 28  ///< packed BGRA 8:8:8:8, 32bpp, BGRABGRA...
     * AV_PIX_FMT_BGR0 - 123 ///< packed BGR 8:8:8,    32bpp, BGRXBGRX...   X=unused/undefined
     *
     * We just to ignore the alpha channel and it's done
     */
    encoder_ctx->pix_fmt = AV_PIX_FMT_BGR0;
    encoder_ctx->width = settings.m_renderWidth;
    encoder_ctx->height = settings.m_renderHeight;
    encoder_ctx->time_base = {std::chrono::steady_clock::period::num,
                              std::chrono::steady_clock::period::den};
    encoder_ctx->framerate = AVRational{settings.m_refreshRate, 1};
    encoder_ctx->sample_aspect_ratio = AVRational{1, 1};
    encoder_ctx->max_b_frames = 0;
    encoder_ctx->gop_size = 30;
    encoder_ctx->bit_rate = settings.mEncodeBitrateMBs * 1000 * 1000;

    err = AVCODEC.avcodec_open2(encoder_ctx, codec, NULL);
    if (err < 0) {
        throw alvr::AvException("Cannot open video encoder codec:", err);
    }

    hw_frame = AVUTIL.av_frame_alloc();
    AVUTIL.av_hwframe_get_buffer(hw_frames_ref, hw_frame, 0);
    AVUTIL.av_buffer_unref(&hw_frames_ref);
}

alvr::EncodePipelineNvEnc::~EncodePipelineNvEnc() {
    for (auto &vk_frame : vk_frames)
        AVUTIL.av_frame_free(&vk_frame);
    AVUTIL.av_buffer_unref(&hw_ctx);
    AVUTIL.av_frame_free(&hw_frame);
}

void alvr::EncodePipelineNvEnc::PushFrame(uint32_t frame_index, bool idr) {
    assert(frame_index < vk_frames.size());

    auto hwframe_transfer_data_start = std::chrono::steady_clock::now();
    int err =
        AVUTIL.av_hwframe_transfer_data(hw_frame, vk_frames[frame_index], 0);
    if (err) {
        throw alvr::AvException("av_hwframe_transfer_data", err);
    }
    auto hwframe_transfer_data_end = std::chrono::steady_clock::now();
    auto hwframe_transfer_data_time = std::chrono::duration_cast<std::chrono::microseconds>(
        hwframe_transfer_data_end - hwframe_transfer_data_start);

    Info("av_hwframe_transfer_data took %d us", hwframe_transfer_data_time.count());

    hw_frame->pict_type = idr ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    hw_frame->pts = std::chrono::steady_clock::now().time_since_epoch().count();

    if ((err = AVCODEC.avcodec_send_frame(encoder_ctx, hw_frame)) < 0) {
        throw alvr::AvException("avcodec_send_frame failed:", err);
    }
}