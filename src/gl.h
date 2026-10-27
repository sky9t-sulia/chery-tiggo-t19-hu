/* Common EGL / OpenGL ES 2 headers. No X11 types: native handles are plain pointers. */
#ifndef HU_GL_H
#define HU_GL_H

#ifndef EGL_NO_X11
#define EGL_NO_X11
#endif
#ifndef MESA_EGL_NO_X11_HEADERS
#define MESA_EGL_NO_X11_HEADERS
#endif
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#endif
