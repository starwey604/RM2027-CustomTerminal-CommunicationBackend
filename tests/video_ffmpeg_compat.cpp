// Exercise the public BGR API with real HEVC I/P/B frames, including delayed output.
#include "rm2027/video/hevc_encoder.h"
#include "rm2027/video/video_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

void roundTrip(const char *name, int frames, int bFrames, bool expectFlushKey,
               bool expectFlushNonKey) {
    rm2027::EncoderConfig config;
    config.width = 128;
    config.height = 96;
    config.fps = 30;
    config.gopSize = 60;
    config.bFrames = bFrames;
    config.extraX265Params = "pools=none:frame-threads=1:open-gop=0:b-adapt=0";
    rm2027::HevcEncoder encoder;
    std::string error;
    require(encoder.open(config, &error), "open encoder: " + error);
    std::vector<uint8_t> bgr(config.width * config.height * 3);
    const uint8_t color[] = {32, 96, 160};
    for (std::size_t i = 0; i < bgr.size(); ++i) bgr[i] = color[i % 3];
    std::vector<std::string> units;
    for (int i = 0; i < frames; ++i) {
        require(encoder.encodeBgr(bgr.data(), config.width, config.height, config.width * 3,
                                 i * 1000000LL / config.fps, &units, &error),
                "encode: " + error);
    }
    require(encoder.flush(&units, &error), "flush encoder: " + error);
    require(units.size() == static_cast<std::size_t>(frames), "encoded frame count");

    rm2027::VideoDecoder decoder;
    require(decoder.open(rm2027::VideoCodec::H265, &error), "open decoder: " + error);
    int decoded = 0, keys = 0, flushedKeys = 0, flushedNonKeys = 0;
    auto checkFrame = [&](bool flushing) {
        require(decoder.frameWidth() == config.width && decoder.frameHeight() == config.height,
                "decoded dimensions");
        require(decoder.frameData() && decoder.frameStride() >= config.width * 3,
                "BGR buffer and stride");
        const bool key = decoder.frameIsKeyframe();
        require(key == (decoded == 0), "key-frame flag must identify only first I frame");
        keys += key;
        flushedKeys += flushing && key;
        flushedNonKeys += flushing && !key;
        for (int y = 0; y < config.height; ++y) {
            const auto *row = decoder.frameData() + y * decoder.frameStride();
            for (int x = 0; x < config.width * 3; ++x)
                require(std::abs(int(row[x]) - int(color[x % 3])) <= 6, "BGR pixel mismatch");
        }
        ++decoded;
    };
    for (const auto &unit : units) {
        // libavcodec bitstream readers require readable zero padding after packet data.
        std::vector<uint8_t> padded(unit.begin(), unit.end());
        padded.resize(unit.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
        auto status = decoder.decode(padded.data(), unit.size(), &error);
        require(status != rm2027::VideoDecoder::Status::Error, "decode: " + error);
        if (status == rm2027::VideoDecoder::Status::FrameReady) checkFrame(false);
    }
    for (int i = 0; i <= frames; ++i) {
        auto status = decoder.flush(&error);
        require(status != rm2027::VideoDecoder::Status::Error, "flush decoder: " + error);
        if (status == rm2027::VideoDecoder::Status::Buffering) break;
        checkFrame(true);
    }
    require(decoded == frames && keys == 1, "decoded frame/key count");
    require(!expectFlushKey || flushedKeys > 0, "flush must exercise a key frame");
    require(!expectFlushNonKey || flushedNonKeys > 0, "flush must exercise a non-key frame");
    require(decoder.flush(&error) == rm2027::VideoDecoder::Status::Buffering,
            "repeated flush must stay drained");
    std::cout << name << ": " << decoded << " frames, " << keys << " key, flush key="
              << flushedKeys << " non-key=" << flushedNonKeys << '\n';
}
} // namespace

int main() {
    try {
        require(avcodec_find_encoder_by_name("libx265"), "test requires FFmpeg libx265 encoder");
        roundTrip("low latency I/P", 9, 0, false, false);
        roundTrip("reordered I/P/B", 9, 2, false, true);
        roundTrip("delayed single I", 1, 2, true, false);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
