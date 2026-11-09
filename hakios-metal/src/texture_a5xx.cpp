// The A5XX (fd5) linear texture layout (see a5xx_layout.hpp for the rules and
// the Mesa source they come from).

#include "hakios-metal/a5xx_layout.hpp"

#include <algorithm>

#include "hakios-metal/texture.hpp"

namespace hakios::metal {

uint64_t texturePitch5(const FormatInfo& format, uint32_t width) {
    // fd5_layout.c:43-47: pitchalign = cpp_shift + 6, so pitch = align(width * cpp,
    // 64 * cpp). Block formats count blocks across, as fdl_set_pitchalign does.
    const uint64_t cpp = format.bytesPerPixel;
    const uint64_t align = 64ull * cpp;
    const uint64_t blocks = std::max<uint64_t>(1, width);
    return (blocks * cpp + align - 1) / align * align;
}

TextureLevel textureLevel5(const FormatInfo& format, uint32_t width, uint32_t height,
                           uint32_t levels, uint32_t level) {
    // The level's width and height, minified (fd5_layout.c:49-55, u_minify).
    const uint32_t w = std::max<uint32_t>(1, width >> level);
    const uint32_t h = std::max<uint32_t>(1, height >> level);
    const uint64_t pitch = texturePitch5(format, w);

    // Rows: the level's height; only the last level's row count is raised to a
    // multiple of 32 so over-fetch on the last level is harmless (fd5_layout.c:59-68).
    // TextureLevel.height is the row count actually stored (as in the A6XX fdl6
    // twin, where the last level's rows are raised to a multiple of 4).
    uint64_t rows = h;
    if (level == levels - 1) {
        rows = (rows + 31) / 32 * 32;
    }

    // Offset: the size of every earlier level (each rows * pitch, no alignment;
    // fd5_layout.c:71, 85-87).
    uint64_t offset = 0;
    for (uint32_t l = 0; l < level; l++) {
        const uint32_t lw = std::max<uint32_t>(1, width >> l);
        const uint32_t lh = std::max<uint32_t>(1, height >> l);
        uint64_t lrows = lh;
        if (l == levels - 1) {
            lrows = (lrows + 31) / 32 * 32;
        }
        offset += lrows * texturePitch5(format, lw);
    }

    return TextureLevel{offset, pitch, w, static_cast<uint32_t>(rows)};
}

uint64_t textureBytes5(const FormatInfo& format, uint32_t width, uint32_t height,
                       uint32_t levels) {
    // The layer is the sum of the levels, 4096-aligned (fd5_layout.c:92-94).
    uint64_t total = 0;
    for (uint32_t l = 0; l < levels; l++) {
        const TextureLevel level = textureLevel5(format, width, height, levels, l);
        total += level.height * level.pitch;  // rows * pitch
    }
    return (total + 4095) / 4096 * 4096;
}

}  // namespace hakios::metal
