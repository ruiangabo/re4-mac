#!/bin/sh
# Builds re4_mac (arm64, native) : the window, renderer, sound and input; the game runs in re4_coreproc.
# Usage: build_native.sh <build dir>   (SDL3 from Homebrew, arm64)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../.." && pwd)
BUILD=$1
CPP="$ROOT/port/android-decomp/app/src/main/cpp"
RT="$ROOT/port/whole/runtime"
OUT="$BUILD/native"; mkdir -p "$OUT/src"
for f in gx_gl.cpp gx_gl_shader.cpp gx_fixed_shaders.h; do
  sed 's/#version 300 es/#version 410 core/g' "$CPP/$f" > "$OUT/src/$f"
done
IMGUI=${IMGUI:-/Volumes/RE4CS/imgui}
INC="-I$OUT/src -I$HERE -I$CPP -I$RT -I$IMGUI -I$IMGUI/backends -I/opt/homebrew/include"
FLAGS="-arch arm64 -std=c++17 -O2 -DGL_SILENCE_DEPRECATION $INC"
for f in gx_gl.cpp gx_gl_shader.cpp; do c++ $FLAGS -c "$OUT/src/$f" -o "$OUT/${f%.cpp}.o"; done
for f in gx_core.cpp gx_renderer.cpp gx_texture.cpp; do c++ $FLAGS -c "$CPP/$f" -o "$OUT/${f%.cpp}.o"; done
for f in core_ipc.cpp core_ipc_client.cpp; do c++ $FLAGS -c "$RT/$f" -o "$OUT/${f%.cpp}.o"; done
mkdir -p "$OUT/imgui"
for f in imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp backends/imgui_impl_sdl3.cpp backends/imgui_impl_opengl3.cpp; do
  o="$OUT/imgui/$(basename "${f%.cpp}").o"
  [ "$o" -nt "$IMGUI/$f" ] || c++ $FLAGS -c "$IMGUI/$f" -o "$o"
done
c++ $FLAGS -c "$HERE/texture_pack.cpp" -o "$OUT/texture_pack.o"
c++ $FLAGS -c "$HERE/viewer_macos.cpp" -o "$OUT/viewer.o"
c++ -arch arm64 -o "$BUILD/re4_mac" "$OUT"/*.o "$OUT"/imgui/*.o -L/opt/homebrew/lib -lSDL3 -framework OpenGL -framework ImageIO -framework CoreGraphics -framework CoreFoundation \
  -Wl,-rpath,/opt/homebrew/lib
echo built "$BUILD/re4_mac"
