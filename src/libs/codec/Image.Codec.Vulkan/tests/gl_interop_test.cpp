/*
 * File: gl_interop_test.cpp
 * Brief: Gl interop test.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// Vulkan→OpenGL 零拷贝互操作集成测试。
// 退出码: 0=PASS  2=SKIP（扩展/设备不支持）  1=FAIL
#include <Image/Codec/Vulkan/vic.hpp>
// 唯一入口 umbrella 头：包含即得 Context/ExportableBuffer/ExportableImage/Compute/
// GlInterop/GlImagePipeline 的全部定义（经 Image.Codec.Vulkan::Api 传播的 PUBLIC
// include 目录解析）。仅 GL 扩展加载头 gl_ext.h 需单独引入。
#include "gl_ext.h"
#include <windows.h>
#include <GL/gl.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <cstdlib>

namespace {

struct GlCtx {
  HWND hwnd = nullptr;
  HDC hdc = nullptr;
  HGLRC rc = nullptr;
  bool create() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"VicGlTestWnd";
    RegisterClassExW(&wc);
    hwnd = CreateWindowExW(0, wc.lpszClassName, L"vic", WS_OVERLAPPED,
                           0, 0, 8, 8, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return false;
    hdc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    int pf = ChoosePixelFormat(hdc, &pfd);
    if (!pf || !SetPixelFormat(hdc, pf, &pfd)) return false;
    rc = wglCreateContext(hdc);
    if (!rc || !wglMakeCurrent(hdc, rc)) return false;
    return true;
  }
  ~GlCtx() {
    if (rc) { wglMakeCurrent(nullptr, nullptr); wglDeleteContext(rc); }
    if (hwnd && hdc) ReleaseDC(hwnd, hdc);
    if (hwnd) DestroyWindow(hwnd);
  }
};

constexpr uint32_t W = 64;
constexpr uint32_t H = 64;

void expectedBuffer(std::vector<uint8_t>& out) {
  out.resize(W * H * 4);
  for (uint32_t y = 0; y < H; ++y)
    for (uint32_t x = 0; x < W; ++x) {
      uint32_t r = x * 255u / (W - 1u);
      uint32_t g = y * 255u / (H - 1u);
      if (((x / 8u) + (y / 8u)) % 2u == 0u) { r ^= 255u; g ^= 255u; }
      size_t o = (size_t)y * W * 4 + x * 4;
      out[o + 0] = static_cast<uint8_t>(r);
      out[o + 1] = static_cast<uint8_t>(g);
      out[o + 2] = 128;
      out[o + 3] = 255;
    }
}

int fail(const char* msg) { std::fprintf(stderr, "FAIL: %s\n", msg); return 1; }
int skip(const char* msg) { std::fprintf(stderr, "SKIP: %s\n", msg); return 2; }

}  // namespace

int main() {
  GlCtx gl;
  if (!gl.create()) return skip("无法创建 OpenGL context");

  const auto& ge = vic::glext::query();
  if (!ge.memoryObject || !ge.memoryObjectWin32)
    return skip("GL 不支持 GL_EXT_memory_object(_win32)");
  if (vic::GlInterop::init() != vic::Result::Ok)
    return skip("GL 扩展函数不可用");

  vic::Context ctx;
  auto r = ctx.init();
  if (r != vic::Result::Ok) return skip("Vulkan 初始化失败（无设备）");
  if (!ctx.deviceInfo().supportsExternalMemoryWin32)
    return skip("Vulkan 不支持 VK_KHR_external_memory_win32");

  vic::GlImagePipeline pipe;
  if (pipe.init(ctx, W, H) != vic::Result::Ok)
    return fail("GlImagePipeline::init 失败");

  // ---- Buffer 通道 ----
  if (pipe.runBuffer() != vic::Result::Ok) return fail("runBuffer 失败");
  const auto& be = vic::glext::query();
  if (!be.glMapNamedBufferRange || !be.glUnmapNamedBuffer)
    return skip("GL 缺 glMapNamedBufferRange");
  GLuint buf = pipe.glBuffer();
  void* ptr = be.glMapNamedBufferRange(buf, 0, (GLsizeiptr)(W * H * 4), GL_MAP_READ_BIT);
  if (!ptr) return fail("glMapNamedBufferRange 失败");
  std::vector<uint8_t> expect;
  expectedBuffer(expect);
  int cmp = std::memcmp(ptr, expect.data(), expect.size());
  be.glUnmapNamedBuffer(buf);
  if (cmp != 0) return fail("Buffer 通道数据不一致（零拷贝验证失败）");
  std::printf("Buffer 通道 PASS：GL 读到与 compute 一致的 %u×%u 数据\n", W, H);

  // ---- Texture 通道 ----
  if (pipe.runTexture() != vic::Result::Ok) return fail("runTexture 失败");
  if (!be.glGetTextureSubImage) return skip("GL 缺 glGetTextureSubImage");
  GLuint tex = pipe.glTexture();
  std::vector<uint8_t> got(W * H * 4);
  be.glGetTextureSubImage(tex, 0, 0, 0, 0, (GLsizei)W, (GLsizei)H, 1,
                          GL_RGBA, GL_UNSIGNED_BYTE, (GLsizei)got.size(), got.data());
  for (uint32_t y = 0; y < H; ++y) {
    for (uint32_t x = 0; x < W; ++x) {
      float fx = float(x) / float(W - 1);
      float fy = float(y) / float(H - 1);
      uint8_t r = static_cast<uint8_t>(fx * 255.0f + 0.5f);
      uint8_t g = static_cast<uint8_t>(fy * 255.0f + 0.5f);
      if ((y / 16) % 2 == 0) r = static_cast<uint8_t>(255 - r);
      size_t o = (size_t)y * W * 4 + x * 4;
      int dr = std::abs((int)got[o + 0] - (int)r);
      int dg = std::abs((int)got[o + 1] - (int)g);
      if (dr > 1 || dg > 1 || std::abs((int)got[o + 2] - 128) > 1) {
        std::fprintf(stderr, "Texture 通道 @(%u,%u) 期望(%u,%u) 实际(%u,%u,%u)\n",
                     x, y, r, g, got[o + 0], got[o + 1], got[o + 2]);
        return fail("Texture 通道数据不一致");
      }
    }
  }
  std::printf("Texture 通道 PASS\n");
  std::printf("ALL PASS\n");
  return 0;
}
