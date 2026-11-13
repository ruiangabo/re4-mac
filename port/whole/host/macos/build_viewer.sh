#!/bin/sh
# Builds re4_viewer_macos (x86_64) next to the CMake build. Usage: build_viewer.sh <build dir> <SDL3 x86_64 prefix>
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../.." && pwd)
BUILD=$1; SDL=$2
CPP="$ROOT/port/android-decomp/app/src/main/cpp"
OUT="$BUILD/viewer"; mkdir -p "$OUT/src"
# GLES 3.0 shaders -> GLSL 4.10 core
for f in gx_gl.cpp gx_gl_shader.cpp gx_fixed_shaders.h; do
  sed 's/#version 300 es/#version 410 core/g' "$CPP/$f" > "$OUT/src/$f"
done
INC="-I$OUT/src -I$HERE -I$CPP -I$ROOT/port/whole/runtime -I$SDL/include"
FLAGS="-arch x86_64 -std=c++17 -O2 -DGL_SILENCE_DEPRECATION $INC"
for f in gx_gl.cpp gx_gl_shader.cpp; do c++ $FLAGS -c "$OUT/src/$f" -o "$OUT/${f%.cpp}.o"; done
for f in gx_core.cpp gx_renderer.cpp gx_texture.cpp; do c++ $FLAGS -c "$CPP/$f" -o "$OUT/${f%.cpp}.o"; done
c++ $FLAGS -c "$HERE/viewer_macos.cpp" -o "$OUT/viewer.o"
c++ -arch x86_64 -Wl,-pagezero_size,0x1000 -o "$BUILD/re4_viewer_macos" "$OUT"/*.o "$SDL/lib/libSDL3.a" \
  -framework OpenGL -framework Cocoa -framework IOKit -framework CoreAudio -framework AudioToolbox -framework CoreFoundation \
  -framework CoreVideo -framework CoreHaptics -framework GameController -framework Metal -framework QuartzCore \
  -framework ForceFeedback -framework Carbon -framework UniformTypeIdentifiers -framework CoreMedia -framework AVFoundation -framework CoreServices -liconv
echo built "$BUILD/re4_viewer_macos"
