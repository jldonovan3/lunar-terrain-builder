#pragma once

#include <cstdint>

namespace lunar::terrain {

enum class ProjectionId : std::uint8_t {
    lunar_qsc_v1 = 1,
};

enum class QuantizationId : std::uint8_t {
    global_u16_0p5m = 1,
};

enum class EncodingProfile : std::uint8_t {
    global_u16 = 1,
    radial_i32_millimeters = 2,
};

enum class ChannelId : std::uint16_t {
    elevation = 0x0001,
    provenance = 0x0002,
    quality = 0x0003,
};

enum class ElementType : std::uint8_t {
    u8 = 1,
    u16 = 2,
    i16 = 3,
    u32 = 4,
    f32 = 5,
    f64 = 6,
    i32 = 7,
    opaque = 255,
};

enum class Codec : std::uint8_t {
    none = 0,
    zstandard = 1,
};

enum class Predictor : std::uint8_t {
    none = 0,
    delta2d_u16 = 1,
};

enum class QualityFlag : std::uint8_t {
    source_interpolated = 1U << 0,
    no_data_filled = 1U << 1,
    fusion_transition = 1U << 2,
    bias_correction = 1U << 3,
    lower_confidence = 1U << 4,
};

}  // namespace lunar::terrain

