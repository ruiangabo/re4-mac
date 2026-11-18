/* GLES3/gl3.h for macOS: the GX renderer (port/android-decomp .../gx_gl.cpp) is written against
 * GLES 3.0; macOS offers OpenGL 4.1 core, which has all of it except the few names below. */
#pragma once
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl3.h>
#include <OpenGL/gl3ext.h>
#include <stdlib.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif
/* ETC2 names (GLES 3 core, GL 4.3; not on macOS GL 4.1). Only the upstream HD pack loader (c4tex, which is
 * never switched on in this port) refers to them, so the values just have to compile. */
#ifndef GL_COMPRESSED_RGB8_ETC2
#define GL_COMPRESSED_RGB8_ETC2 0x9274
#define GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2 0x9276
#define GL_COMPRESSED_RGBA8_ETC2_EAC 0x9278
#endif
/* GL 4.3 (not on macOS): a hint only */
static inline void glInvalidateFramebuffer(GLenum t, GLsizei n, const GLenum* a) { (void) t; (void) n; (void) a; }

/* Core profile: glGetString(GL_EXTENSIONS) is gone (NULL); the renderer looks for extension names in one
 * string as GLES gives it, so join glGetStringi's list once. */
static inline const GLubyte* re4_glGetString(GLenum name) {
    if (name != GL_EXTENSIONS) return glGetString(name);
    static char* joined = 0;
    if (!joined) {
        GLint n = 0; glGetIntegerv(GL_NUM_EXTENSIONS, &n);
        size_t len = 1;
        for (GLint i = 0; i < n; i++) len += strlen((const char*) glGetStringi(GL_EXTENSIONS, (GLuint) i)) + 1;
        joined = (char*) malloc(len); joined[0] = 0;
        for (GLint i = 0; i < n; i++) { strcat(joined, (const char*) glGetStringi(GL_EXTENSIONS, (GLuint) i)); strcat(joined, " "); }
    }
    return (const GLubyte*) joined;
}
#define glGetString re4_glGetString
#ifdef __cplusplus
}
#endif
