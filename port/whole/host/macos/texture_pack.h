// Texture packs (Dolphin naming) and the texture dump of the macOS front end (texture_pack.cpp).
#pragma once
#include <string>
#include <vector>

namespace re4gx { struct GxTexHookInfo; }

namespace re4tex {
// Indexes the replacement pictures (tex1_*.png, any subfolder) of `folders`, the first folder winning, and
// installs the renderer's hooks; `dumpFolder` non-empty: every texture uploaded from now on is saved there once.
// Returns the replacements found. Again with other folders: takes effect for textures uploaded after it.
// maxTextureSize: 0 = the pictures as they are; else their longer side is scaled down to it.
unsigned Setup(const std::vector<std::string>& folders, const std::string& dumpFolder, unsigned maxTextureSize = 0);
// The Dolphin name of a texture (tests).
std::string NameFor(const re4gx::GxTexHookInfo& info);
// The hash against its reference values.
bool SelfTest();
}  // namespace re4tex
