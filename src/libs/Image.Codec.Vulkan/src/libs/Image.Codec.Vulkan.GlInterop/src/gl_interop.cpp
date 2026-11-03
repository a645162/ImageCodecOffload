/*
 * File: gl_interop.cpp
 * Brief: Gl interop.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

#include <Image/Codec/Vulkan/vic.hpp>
#include "gl_ext.h"
#include "gl_interop.h"
#include <cstring>
#include <vector>

namespace vic {
namespace glext {

namespace {
Ext g_ext;
bool g_init = false;

bool hasExtension(const char* ext) {
  const char* s = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
  if (!s) return false;
  size_t len = std::strlen(ext);
  const char* p = s;
  while ((p = std::strstr(p, ext)) != nullptr) {
    if ((p == s || p[-1] == ' ') && (p[len] == '\0' || p[len] == ' ')) return true;
    p += len;
  }
  return false;
}

template <typename T>
T proc(const char* name) {
  return reinterpret_cast<T>(wglGetProcAddress(name));
}
}  // namespace

const Ext& query() {
  if (g_init) return g_ext;
  g_init = true;
  g_ext.gl45 = (glGetString(GL_VERSION) != nullptr);
  g_ext.memoryObject = hasExtension("GL_EXT_memory_object");
  g_ext.memoryObjectWin32 = hasExtension("GL_EXT_memory_object_win32");
  g_ext.glCreateBuffers = proc<glCreateBuffers_t>("glCreateBuffers");
  g_ext.glCreateTextures = proc<glCreateTextures_t>("glCreateTextures");
  g_ext.glDeleteBuffers = proc<glDeleteBuffers_t>("glDeleteBuffers");
  g_ext.glDeleteTextures = proc<glDeleteTextures_t>("glDeleteTextures");
  g_ext.glMapNamedBufferRange = proc<glMapNamedBufferRange_t>("glMapNamedBufferRange");
  g_ext.glUnmapNamedBuffer = proc<glUnmapNamedBuffer_t>("glUnmapNamedBuffer");
  g_ext.glGetTextureSubImage = proc<glGetTextureSubImage_t>("glGetTextureSubImage");
  g_ext.glCreateMemoryObjectsEXT = proc<glCreateMemoryObjectsEXT_t>("glCreateMemoryObjectsEXT");
  g_ext.glDeleteMemoryObjectsEXT = proc<glDeleteMemoryObjectsEXT_t>("glDeleteMemoryObjectsEXT");
  g_ext.glImportMemoryWin32HandleEXT = proc<glImportMemoryWin32HandleEXT_t>("glImportMemoryWin32HandleEXT");
  g_ext.glNamedBufferStorageMemEXT = proc<glNamedBufferStorageMemEXT_t>("glNamedBufferStorageMemEXT");
  g_ext.glTextureStorageMem2DEXT = proc<glTextureStorageMem2DEXT_t>("glTextureStorageMem2DEXT");
  return g_ext;
}

}  // namespace glext

namespace {

struct Imported {
  GLuint object = 0;
  GLuint memory = 0;
  bool isTexture = false;
};

std::vector<Imported>& imports() {
  static std::vector<Imported> v;
  return v;
}

}  // namespace

Result GlInterop::init() {
  const auto& e = glext::query();
  if (!e.gl45) return Result::InitFailed;
  if (!e.memoryObject || !e.memoryObjectWin32) return Result::UnsupportedExtension;
  if (!e.glCreateBuffers || !e.glCreateMemoryObjectsEXT || !e.glImportMemoryWin32HandleEXT ||
      !e.glNamedBufferStorageMemEXT || !e.glTextureStorageMem2DEXT)
    return Result::UnsupportedExtension;
  return Result::Ok;
}

bool GlInterop::available() {
  return init() == Result::Ok;
}

uint32_t GlInterop::importBuffer(void* handle, uint64_t size) {
  const auto& e = glext::query();
  if (!e.glCreateBuffers || !e.glCreateMemoryObjectsEXT || !e.glImportMemoryWin32HandleEXT ||
      !e.glNamedBufferStorageMemEXT)
    return 0;
  GLuint mem = 0;
  e.glCreateMemoryObjectsEXT(1, &mem);
  e.glImportMemoryWin32HandleEXT(mem, static_cast<GLuint64>(size), glext::kOpaqueWin32Handle, handle);
  GLuint buf = 0;
  e.glCreateBuffers(1, &buf);
  e.glNamedBufferStorageMemEXT(buf, static_cast<GLsizeiptr>(size), mem, 0);
  if (glGetError() != GL_NO_ERROR) {
    // 失败路径：删除已创建对象，不入册 imports()（避免泄漏），返回 0。
    if (buf) e.glDeleteBuffers(1, &buf);
    if (mem && e.glDeleteMemoryObjectsEXT) e.glDeleteMemoryObjectsEXT(1, &mem);
    return 0;
  }
  imports().push_back({buf, mem, false});
  return static_cast<uint32_t>(buf);
}

uint32_t GlInterop::importTexture(void* handle, uint32_t w, uint32_t h, uint64_t size) {
  const auto& e = glext::query();
  if (!e.glCreateTextures || !e.glCreateMemoryObjectsEXT || !e.glImportMemoryWin32HandleEXT ||
      !e.glTextureStorageMem2DEXT)
    return 0;
  GLuint mem = 0;
  e.glCreateMemoryObjectsEXT(1, &mem);
  // size 必须为内存真实字节数（GL_EXT_memory_object_win32），传 0 会导致
  // 驱动在 glTextureStorageMem2DEXT 绑定时崩溃（vcpkg loader + NVIDIA 实测）。
  e.glImportMemoryWin32HandleEXT(mem, static_cast<GLuint64>(size), glext::kOpaqueWin32Handle, handle);
  GLuint tex = 0;
  e.glCreateTextures(GL_TEXTURE_2D, 1, &tex);
  e.glTextureStorageMem2DEXT(tex, 1, GL_RGBA8, static_cast<GLsizei>(w), static_cast<GLsizei>(h), mem, 0);
  if (glGetError() != GL_NO_ERROR) {
    // 失败路径：删除已创建对象，不入册 imports()（避免泄漏），返回 0。
    if (tex) e.glDeleteTextures(1, &tex);
    if (mem && e.glDeleteMemoryObjectsEXT) e.glDeleteMemoryObjectsEXT(1, &mem);
    return 0;
  }
  imports().push_back({tex, mem, true});
  return static_cast<uint32_t>(tex);
}

void GlInterop::releaseObject(uint32_t obj, bool isTexture) {
  const auto& e = glext::query();
  auto& v = imports();
  for (auto it = v.begin(); it != v.end(); ++it) {
    if (it->object == obj && it->isTexture == isTexture) {
      if (isTexture && e.glDeleteTextures) e.glDeleteTextures(1, &it->object);
      else if (!isTexture && e.glDeleteBuffers) e.glDeleteBuffers(1, &it->object);
      if (e.glDeleteMemoryObjectsEXT) e.glDeleteMemoryObjectsEXT(1, &it->memory);
      v.erase(it);
      return;
    }
  }
}

}  // namespace vic
