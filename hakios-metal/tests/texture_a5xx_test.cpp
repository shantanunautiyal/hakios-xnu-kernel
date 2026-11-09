// The A5XX (fd5) texture layout, against values worked out by hand from Mesa's
// fd5_layout.c (tools/mesa/a5xx/fd5_layout.c): pitch = align(width * cpp,
// 64 * cpp), last level's rows to a multiple of 32, the layer 4096-aligned.

#include "hakios-metal/a5xx_layout.hpp"
#include "hakios-metal/texture.hpp"
#include "test.hpp"

using namespace hakios::metal;

namespace {

const FormatInfo* rgba8() { return formatInfo(PixelFormat::kRGBA8Unorm); }
const FormatInfo* r8() { return formatInfo(PixelFormat::kR8Unorm); }

}  // namespace

TEST(texture_a5xx_single_level) {
    // RGBA8, 64 x 4: pitch = align(64*4, 64*4) = 256; rows = align(4, 32) = 32;
    // bytes = align(32*256, 4096) = 8192.
    CHECK(texturePitch5(*rgba8(), 64) == 256);
    CHECK(textureBytes5(*rgba8(), 64, 4) == 8192);
    const TextureLevel l = textureLevel5(*rgba8(), 64, 4, 1, 0);
    CHECK(l.offset == 0 && l.pitch == 256 && l.width == 64 && l.height == 32);

    // RGBA8, 33 x 33: pitch = align(132, 256) = 256; rows = align(33, 32) = 64;
    // bytes = 64 * 256 = 16384.
    CHECK(texturePitch5(*rgba8(), 33) == 256);
    CHECK(textureBytes5(*rgba8(), 33, 33) == 16384);

    // RGBA8, 1 x 1: pitch = align(4, 256) = 256; rows = align(1, 32) = 32;
    // bytes = 8192.
    CHECK(texturePitch5(*rgba8(), 1) == 256);
    CHECK(textureBytes5(*rgba8(), 1, 1) == 8192);

    // R8, 64 x 4: pitch = align(64, 64) = 64; rows = align(4, 32) = 32;
    // bytes = align(2048, 4096) = 4096.
    CHECK(texturePitch5(*r8(), 64) == 64);
    CHECK(textureBytes5(*r8(), 64, 4) == 4096);

    // RGBA8, 65 x 1: pitch = align(260, 256) = 512; rows = 32; bytes = 16384.
    CHECK(texturePitch5(*rgba8(), 65) == 512);
    CHECK(textureBytes5(*rgba8(), 65, 1) == 16384);
}

TEST(texture_a5xx_mip_levels) {
    // RGBA8, 64 x 64, 2 levels:
    //   L0: width 64, rows 64, pitch 256 -> offset 0, size 16384
    //   L1: width 32, rows align(32, 32) = 32, pitch align(128, 256) = 256
    //        -> offset 16384, size 8192
    //   layer = align(16384 + 8192, 4096) = 24576
    const TextureLevel l0 = textureLevel5(*rgba8(), 64, 64, 2, 0);
    CHECK(l0.offset == 0 && l0.pitch == 256 && l0.width == 64 && l0.height == 64);
    const TextureLevel l1 = textureLevel5(*rgba8(), 64, 64, 2, 1);
    CHECK(l1.offset == 16384 && l1.pitch == 256 && l1.width == 32 && l1.height == 32);
    CHECK(textureBytes5(*rgba8(), 64, 64, 2) == 24576);

    // A middle level's rows are not aligned; only the last one is. With 3 levels
    // of 128 x 128: L0 pitch align(512, 256) = 512, rows 128; L1 width 64,
    // pitch align(256, 256) = 256, rows 64 (not aligned); L2 width 32,
    // pitch align(128, 256) = 256, rows align(32, 32) = 32.
    const uint64_t pitch0 = texturePitch5(*rgba8(), 128);  // 512
    CHECK(pitch0 == 512);
    const uint64_t l0size = 128 * pitch0;                 // 65536
    const TextureLevel m1 = textureLevel5(*rgba8(), 128, 128, 3, 1);
    CHECK(m1.offset == l0size && m1.height == 64 && m1.pitch == 256);
    const TextureLevel m2 = textureLevel5(*rgba8(), 128, 128, 3, 2);
    CHECK(m2.offset == l0size + 64 * 256 && m2.height == 32);
    const uint64_t expect = l0size + 64 * 256 + 32 * 256;  // 90112, already 4096-aligned
    CHECK(textureBytes5(*rgba8(), 128, 128, 3) == expect);
}
