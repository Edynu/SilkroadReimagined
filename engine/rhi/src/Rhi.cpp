#include "sr/rhi/Rhi.h"

#include <algorithm>

namespace sr::rhi {

bool IsBlockCompressed(Format format) {
    return format == Format::BC1Unorm || format == Format::BC2Unorm || format == Format::BC3Unorm;
}

std::size_t MipSizeInBytes(Format format, std::uint32_t width, std::uint32_t height) {
    width = std::max(width, 1u);
    height = std::max(height, 1u);
    switch (format) {
    case Format::BC1Unorm:
        return std::size_t{(width + 3) / 4} * ((height + 3) / 4) * 8;
    case Format::BC2Unorm:
    case Format::BC3Unorm:
        return std::size_t{(width + 3) / 4} * ((height + 3) / 4) * 16;
    case Format::RGBA8Unorm:
    case Format::BGRA8Unorm:
    case Format::D32Float:
        return std::size_t{width} * height * 4;
    case Format::RGBA16Float:
        return std::size_t{width} * height * 8;
    case Format::Unknown:
        break;
    }
    return 0;
}

} // namespace sr::rhi
