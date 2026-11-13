// GameCube texture formats (GX, tiled, big-endian) to RGBA8 for the GX
// renderer of the core mode (gx_gl.cpp, port/docs/gx-plan.md step G3).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace re4gx {

// GX texture formats (BP TX_SETIMAGE0 bits 20-23).
enum GxTexFormat : unsigned {
  kTexI4 = 0,
  kTexI8 = 1,
  kTexIA4 = 2,
  kTexIA8 = 3,
  kTexRGB565 = 4,
  kTexRGB5A3 = 5,
  kTexRGBA8 = 6,
  kTexC4 = 8,
  kTexC8 = 9,
  kTexC14X2 = 10,
  kTexCMPR = 14
};

// TLUT formats (BP TX_SETTLUT bits 10-11).
enum GxTlutFormat : unsigned { kTlutIA8 = 0, kTlutRGB565 = 1, kTlutRGB5A3 = 2 };

bool GxIsPaletted(unsigned format);
// Palette entries a paletted format indexes (16, 256, 16384).
unsigned GxPaletteEntries(unsigned format);
// Bytes of one mipmap level (whole tiles).
std::size_t GxTextureLevelBytes(unsigned format, unsigned width, unsigned height);

// One level at `data` (`bytes` available) into width*height RGBA8 texels, top
// row first. Paletted formats look up `tlut` (big-endian entries as TMEM holds
// them, `tlutEntries` of them; missing entries read as 0). False when the data
// is short or the format unknown (the texels are then left black).
bool GxDecodeTexture(const std::uint8_t *data, std::size_t bytes, unsigned format, unsigned width,
                     unsigned height, const std::uint8_t *tlut, unsigned tlutEntries,
                     unsigned tlutFormat, std::vector<std::uint8_t> &rgba);

// Texture packs (the macOS front end, host/macos/texture_pack.cpp): GxCore::Upload asks the hook before it
// decodes a texture. `data` is level 0 (`bytes` of it, whole tiles), `tlut` the palette (paletted formats).
struct GxTexHookInfo {
  const std::uint8_t *data = nullptr;
  std::size_t bytes = 0;
  unsigned format = 0, width = 0, height = 0, levels = 1;
  const std::uint8_t *tlut = nullptr;
  unsigned tlutEntries = 0, tlutFormat = 0;
  std::uint64_t key = 0;  // the renderer's key of the texture
  unsigned ticket = 0;    // this upload of it (a replacement read in the background comes back with it)
};
// A replacement: RGBA8 levels, level 0 first, each sizes[i] = {w, h}.
struct GxTexReplacement {
  std::vector<std::vector<std::uint8_t>> levels;
  std::vector<std::pair<unsigned, unsigned>> sizes;
  bool pending = false;  // false returned, but a replacement is being read: gGxTextureReady gives it later
  unsigned compressed = 0;  // 0: RGBA8 levels; 1: BC1 (opaque); 3: BC3 (the levels hold 4x4 blocks)
};
// The renderer takes BC1/BC3 replacements (GL_EXT_texture_compression_s3tc; set by the renderer at start).
extern bool gGxTextureBc;
// A replacement read in the background, for the texture uploaded as `key` with `ticket`.
struct GxTexReady {
  std::uint64_t key = 0;
  unsigned ticket = 0;
  GxTexReplacement rep;
};
// true: `out` replaces the texture. Null (the default): no packs.
extern bool (*gGxTextureReplace)(const GxTexHookInfo &info, GxTexReplacement *out);
// One finished background replacement into `out` (false: none now). Null: replacements only as uploaded.
extern bool (*gGxTextureReady)(GxTexReady *out);
// the decoded level 0 (width x height RGBA8), for dumping. Null: none.
extern void (*gGxTextureDump)(const GxTexHookInfo &info, const std::uint8_t *rgba);

} // namespace re4gx
