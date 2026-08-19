#ifndef EAVP_MEDIA_VIDEO_FORMAT_HPP_
#define EAVP_MEDIA_VIDEO_FORMAT_HPP_

#include <vector>

#include "eavp/media/buffer.hpp"

namespace eavp {

enum class PixelFormat {
    kUnknown,
    kNv12,
    kYuv420p,
    kRgb24,
};

enum class ColorRange {
    kUnknown,
    kLimited,
    kFull,
};

enum class ColorPrimaries {
    kUnknown,
    kBt601,
    kBt709,
    kBt2020,
};

enum class TransferCharacteristic {
    kUnknown,
    kBt709,
    kSrgb,
    kPq,
    kHlg,
};

enum class MatrixCoefficients {
    kUnknown,
    kBt601,
    kBt709,
    kBt2020Ncl,
};

class VideoFormat {
public:
    static Result<VideoFormat> create(PixelFormat pixel_format, int width, int height,
                                      MemoryDomain memory_domain,
                                      const std::vector<PlaneLayout>& planes,
                                      ColorRange color_range = ColorRange::kUnknown,
                                      ColorPrimaries color_primaries = ColorPrimaries::kUnknown,
                                      TransferCharacteristic transfer =
                                          TransferCharacteristic::kUnknown,
                                      MatrixCoefficients matrix =
                                          MatrixCoefficients::kUnknown);

    PixelFormat pixel_format() const { return pixel_format_; }
    int width() const { return width_; }
    int height() const { return height_; }
    MemoryDomain memory_domain() const { return memory_domain_; }
    const std::vector<PlaneLayout>& planes() const { return planes_; }
    ColorRange color_range() const { return color_range_; }
    ColorPrimaries color_primaries() const { return color_primaries_; }
    TransferCharacteristic transfer() const { return transfer_; }
    MatrixCoefficients matrix() const { return matrix_; }

private:
    VideoFormat(PixelFormat pixel_format, int width, int height, MemoryDomain memory_domain,
                const std::vector<PlaneLayout>& planes, ColorRange color_range,
                ColorPrimaries color_primaries, TransferCharacteristic transfer,
                MatrixCoefficients matrix)
        : pixel_format_(pixel_format),
          width_(width),
          height_(height),
          memory_domain_(memory_domain),
          planes_(planes),
          color_range_(color_range),
          color_primaries_(color_primaries),
          transfer_(transfer),
          matrix_(matrix) {}

    PixelFormat pixel_format_;
    int width_;
    int height_;
    MemoryDomain memory_domain_;
    std::vector<PlaneLayout> planes_;
    ColorRange color_range_;
    ColorPrimaries color_primaries_;
    TransferCharacteristic transfer_;
    MatrixCoefficients matrix_;
};

}  // namespace eavp

#endif  // EAVP_MEDIA_VIDEO_FORMAT_HPP_
