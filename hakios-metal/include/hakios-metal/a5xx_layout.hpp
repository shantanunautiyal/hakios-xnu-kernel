/*
 * a5xx_layout.hpp: the Adreno 506 (A5XX) linear-texture layout, the fd5
 * twin of texture.hpp's fdl6 rules. Mesa's A5XX layout is
 * src/freedreno/fdl/fd5_layout.c (vendored reference in
 * tools/mesa/a5xx/fd5_layout.c); the differences from fdl6 are:
 *
 *   - the pitch rule is the same: pitch = align(width * cpp, 64 * cpp)
 *     (fd5_layout.c:43-47, pitchalign = cpp_shift + 6);
 *   - a linear image's rows are NOT 4-aligned as in fdl6: only the LAST
 *     level's row count is raised to a multiple of 32 (fd5_layout.c:59-68);
 *   - each level's size is rows * pitch with no per-level 4096 alignment
 *     (fd5_layout.c:85-87); only the whole layer is 4096-aligned
 *     (layer_size, fd5_layout.c:92-94).
 *
 * These functions mirror texture.hpp's texturePitch/textureBytes/textureLevel
 * so a 506 texture can be laid out and validated on the host. Only the
 * linear 2D case is implemented (what the render/compute path needs first);
 * tiled and 3D variants come with the dispatch/render work.
 */
#pragma once

#include <cstdint>

#include "hakios-metal/texture.hpp"  // FormatInfo, TextureLevel

namespace hakios::metal {

// fd5 pitch: align(width * cpp, 64 * cpp) -- the same rule as texturePitch.
uint64_t texturePitch5(const FormatInfo& format, uint32_t width);

// The whole 2D layer: levels one after another (offset), each level's rows
// (last level raised to a multiple of 32), the layer 4096-aligned.
uint64_t textureBytes5(const FormatInfo& format, uint32_t width, uint32_t height,
                       uint32_t levels = 1);

// Level L of the fd5 layout (fd5_layout.c:49-90).
TextureLevel textureLevel5(const FormatInfo& format, uint32_t width, uint32_t height,
                          uint32_t levels, uint32_t level);

}  // namespace hakios::metal
