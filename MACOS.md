# CubeEvil4 for macOS (Apple Silicon) — experimental

An unofficial, non-commercial fan port of the Resident Evil 4 GameCube decompilation to macOS on
Apple Silicon. It is built on top of [CubeEvil4-Android](https://github.com/Andiweli/CubeEvil4-Android)
by Andiweli, which is itself a fork of the
[Resident Evil 4 GameCube decompilation](https://github.com/adonis-singh/re4) by adonis-singh.
All the hard work (the decompilation, the platform layer, the GX renderer) is theirs; this port only adds
a macOS front end on top. Please read their READMEs and give them the credit.

It is **not affiliated with, endorsed by or sponsored by Capcom, Nintendo or CRI Middleware**.
It contains **no game data**: you need your own images of both discs of your own copy.
If you do not own the game, please buy the *Resident Evil 4* remake or an official re-release.

## Status

Playable. 30 Hz game logic with interpolated 60 fps output, controller and keyboard/mouse support,
in-game settings menu, optional texture packs (Dolphin naming, for example the HD Project).
Tested on Apple Silicon only (M1). Not every room has been tested on macOS.

## It is a hybrid, not 100% native

| Part | Architecture |
|---|---|
| Front end: window, OpenGL 4.1 renderer (on Apple's Metal-backed GL), audio, input, menu | **arm64, native** |
| Game core (the decompiled game code) | **x86_64, runs under Rosetta 2** |

The two are separate processes that talk through shared memory.

Why the game core is x86_64: the game expects its RAM at the fixed address `0x80000000` and stores
pointers as 32-bit values. On arm64, macOS reserves the first 4 GB of every process's address space, so
that address cannot be mapped. Under Rosetta it can (`-pagezero_size 0x1000`).

Because of this the port **needs Rosetta 2**, and Apple has said it will phase Rosetta out in future
macOS versions. The roadmap item is an arm64 game core (for example a 4 GB-aligned memory arena so the
existing pointer truncation keeps working, or a 32-bit sandbox). Contributions are welcome.

## What you need

- A Mac with Apple Silicon, Rosetta 2 (`softwareupdate --install-rosetta --agree-to-license`)
- Homebrew packages: `cmake ninja lld sdl3`
- A **case-sensitive** volume for the source tree (the repo has both `src/Tools` and `src/tools`).
  A case-sensitive APFS sparse image works: `hdiutil create -size 8g -type SPARSE -fs "Case-sensitive APFS" -volname RE4CS re4cs`
- [Dear ImGui](https://github.com/ocornut/imgui) v1.91.9 checked out somewhere (`IMGUI=/path/to/imgui`)
- Your own game data: the extracted file systems of both discs, in a folder `disc-data/` (see below)

## Build

```sh
# 1. the game core (x86_64, Release is important: -O0 is far too slow)
cmake -S port/whole -B build-mac-x64 -G Ninja \
  -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_BUILD_TYPE=Release
ninja -C build-mac-x64 -k 0     # a few unrelated targets fail; -k 0 keeps going

# 2. the native front end (arm64)
IMGUI=/path/to/imgui sh port/whole/host/macos/build_native.sh build-mac-x64

# 3. optional: package "Resident Evil 4.app" (no game data inside)
sh port/whole/host/macos/package_app.sh build-mac-x64 .
```

## Your game data

Extract the file systems of your own two discs (the port plays the debug build `G4BE08`, and
the retail releases, as the Android port does; see its README for the supported releases) into:

```
disc-data/
  disc0/   (sys/ and files/ of disc 1)
  disc1/   (sys/ and files/ of disc 2)
```

The app asks for this folder the first time and remembers it. Run the front end directly with:

```sh
build-mac-x64/re4_mac build-mac-x64/libre4_main.dylib build-mac-x64/modules /path/to/disc-data
```

## Controls (defaults)

WASD move, mouse aim, J/K/U/I = A/B/X/Y, Enter = Start, arrows = D-pad, Esc = settings menu, F = fullscreen,
H = toggle 60 fps interpolation. PlayStation controllers work; button prompts can be replaced by the
included PlayStation icon texture pack.

Settings live in `~/Library/Application Support/RE4 Mac/settings.ini`; the log of the last run is in
`~/Library/Application Support/RE4 Mac/last-run.log`.

## Credits

- **adonis-singh** — the Resident Evil 4 GameCube decompilation
- **Andiweli** — CubeEvil4, the Android port (platform layer, GX renderer, core API) this port is built on
- **Capcom** — Resident Evil 4, all of its code, art, music and design
- **Nintendo, CRI Middleware** — the SDK and middleware the game is built on

## License

Same scope as upstream: the build scripts, tools, configuration, documentation and the code written for
this macOS port are released under CC0 1.0. This does **not** cover the reconstructed game, SDK and
middleware source in `src/` and `include/`, which remains the property of Capcom, Nintendo and CRI Middleware
and is published for research and preservation only.
