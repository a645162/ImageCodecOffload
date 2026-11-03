/*
 * File: gl_ext.h
 * Brief: Gl ext.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

#pragma once
// GL 4.5 core + GL_EXT_memory_object(_win32) 函数指针
// （Windows opengl32 只导出 1.1，扩展一律经 wglGetProcAddress 获取）。
#include <windows.h>
#include <GL/gl.h>
#include <cstddef>

// Windows SDK 的 gl.h（GL 1.1）还缺 GL 1.5 的 GL_MAP_READ_BIT 与 GL 3.0 的
// GL_RGBA8。按 Khronos 定义补齐，guard 防止与未来真实定义冲突。
#ifndef GL_MAP_READ_BIT
#define GL_MAP_READ_BIT 0x0001
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif

// Windows SDK 的 gl.h 只提供 GL 1.1 类型，缺 GL 1.5 的 ptr 类型与
// GL 3.0 的 64 位类型（Khronos glext.h 中定义）。此处按 Khronos 定义补齐。
typedef std::ptrdiff_t GLintptr;
typedef std::ptrdiff_t GLsizeiptr;
typedef unsigned long long GLuint64;

namespace vic {
namespace glext {

using glCreateBuffers_t = void(APIENTRY*)(GLsizei, GLuint*);
using glCreateTextures_t = void(APIENTRY*)(GLenum, GLsizei, GLuint*);
using glDeleteBuffers_t = void(APIENTRY*)(GLsizei, const GLuint*);
using glDeleteTextures_t = void(APIENTRY*)(GLsizei, const GLuint*);
using glMapNamedBufferRange_t = void*(APIENTRY*)(GLuint, GLintptr, GLsizeiptr, GLbitfield);
using glUnmapNamedBuffer_t = GLboolean(APIENTRY*)(GLuint);
using glGetTextureSubImage_t = void(APIENTRY*)(GLuint, GLint, GLint, GLint, GLint,
                                               GLsizei, GLsizei, GLsizei, GLenum, GLenum,
                                               GLsizei, void*);
using glCreateMemoryObjectsEXT_t = void(APIENTRY*)(GLsizei, GLuint*);
using glDeleteMemoryObjectsEXT_t = void(APIENTRY*)(GLsizei, const GLuint*);
using glImportMemoryWin32HandleEXT_t = void(APIENTRY*)(GLuint, GLuint64, GLenum, void*);
using glNamedBufferStorageMemEXT_t = void(APIENTRY*)(GLuint, GLsizeiptr, GLuint, GLuint64);
using glTextureStorageMem2DEXT_t = void(APIENTRY*)(GLuint, GLsizei, GLenum, GLsizei,
                                                   GLsizei, GLuint, GLuint64);

constexpr GLenum kOpaqueWin32Handle = 0x9587;  // GL_HANDLE_TYPE_OPAQUE_WIN32_EXT

struct Ext {
  bool gl45 = false;
  bool memoryObject = false;
  bool memoryObjectWin32 = false;
  glCreateBuffers_t glCreateBuffers = nullptr;
  glCreateTextures_t glCreateTextures = nullptr;
  glDeleteBuffers_t glDeleteBuffers = nullptr;
  glDeleteTextures_t glDeleteTextures = nullptr;
  glMapNamedBufferRange_t glMapNamedBufferRange = nullptr;
  glUnmapNamedBuffer_t glUnmapNamedBuffer = nullptr;
  glGetTextureSubImage_t glGetTextureSubImage = nullptr;
  glCreateMemoryObjectsEXT_t glCreateMemoryObjectsEXT = nullptr;
  glDeleteMemoryObjectsEXT_t glDeleteMemoryObjectsEXT = nullptr;
  glImportMemoryWin32HandleEXT_t glImportMemoryWin32HandleEXT = nullptr;
  glNamedBufferStorageMemEXT_t glNamedBufferStorageMemEXT = nullptr;
  glTextureStorageMem2DEXT_t glTextureStorageMem2DEXT = nullptr;
};

const Ext& query();  // 单例，惰性探测

}  // namespace glext
}  // namespace vic
