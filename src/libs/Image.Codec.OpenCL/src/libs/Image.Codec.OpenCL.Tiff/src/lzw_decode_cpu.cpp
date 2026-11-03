/*
 * File: lzw_decode_cpu.cpp
 * Brief: Lzw decode cpu.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-08-12
 */

// tiff/lzw_decode_cpu.cpp - CPU LZW 解码实现。
//
// 字符串表用「前缀链」表示：code -> (prefix_code, suffix_byte)。
// 0..255 为字面量（prefix=无），258..4095 为组合条目。输出时沿链收集 suffix
// 到栈再反转，避免逐字节拷贝。
#include "lzw_decode_cpu.h"

namespace image_codec {
namespace tiff {

namespace {

constexpr uint16_t kClear = 256;
constexpr uint16_t kEoi = 257;
constexpr uint16_t kFirst = 258;   // 第一个空闲码
constexpr uint16_t kMaxEnt = 4096; // 字典容量
constexpr uint16_t kNoPrefix = 0xFFFF;

}  // namespace

int lzwDecode(const uint8_t* in, size_t in_size, std::vector<uint8_t>& out) {
  out.clear();
  if (in == nullptr && in_size > 0) return -1;

  uint16_t prefix[kMaxEnt];
  uint8_t suffix[kMaxEnt];
  for (uint16_t i = 0; i < 256; ++i) {
    prefix[i] = kNoPrefix;
    suffix[i] = static_cast<uint8_t>(i);
  }

  uint32_t nextdata = 0;
  int nextbits = 0;
  size_t in_pos = 0;
  int nbits = 9;
  int next_code = kFirst;
  int old = -1;

  // 单条字符串最长不超过字典条目数（4096），留余量。
  uint8_t stack[4096 + 32];

  auto getNextCode = [&]() -> int {
    while (nextbits < nbits) {
      if (in_pos >= in_size) return -1;  // 数据不足
      nextdata = (nextdata << 8) | in[in_pos++];
      nextbits += 8;
    }
    const int code =
        static_cast<int>((nextdata >> (nextbits - nbits)) & ((1u << nbits) - 1u));
    nextbits -= nbits;
    return code;
  };

  // 宽度增长：在 next_code 到达 511/1023/2047 时切换（比编码器早一位）。
  // 原因：ClearCode 后第一个码是字面量、不建表，解码器建表始终滞后编码器一位；
  // 编码器在 next_code==512/1024/2048 切换，两者配合才能与码流同步。
  // 这也与 libtiff 解码器的 `++free_entp > maxcodep`(maxcodep=MAXCODE-1) 一致。
  auto widthUp = [&]() {
    if (next_code == 511) nbits = 10;
    else if (next_code == 1023) nbits = 11;
    else if (next_code == 2047) nbits = 12;
  };

  for (;;) {
    const int code = getNextCode();
    if (code < 0) return -2;  // 位流提前结束（缺 EOI）

    if (code == kEoi) break;
    if (code == kClear) {
      next_code = kFirst;
      nbits = 9;
      old = -1;
      continue;
    }
    if (code >= kMaxEnt) return -3;

    if (old == -1) {
      // ClearCode 之后第一个码必须是字面量。
      if (code >= 256) return -4;
      out.push_back(static_cast<uint8_t>(code));
      old = code;
      continue;
    }

    // 把当前码的字符串沿链收集到 stack（栈顶为串尾）。
    size_t sp = 0;
    if (code < next_code) {
      int c = code;
      while (prefix[c] != kNoPrefix) {
        stack[sp++] = suffix[c];
        c = prefix[c];
      }
      stack[sp++] = suffix[c];
      const uint8_t first_byte = stack[sp - 1];
      // 反转输出 dict[code]
      for (size_t i = sp; i-- > 0;) out.push_back(stack[i]);
      // 新条目 = dict[old] + first(dict[code])
      if (next_code < kMaxEnt) {
        prefix[next_code] = static_cast<uint16_t>(old);
        suffix[next_code] = first_byte;
        next_code++;
        widthUp();
      }
    } else if (code == next_code) {
      // KwKwK：新条目 = dict[old] + first(dict[old])；输出同新条目。
      int c = old;
      while (prefix[c] != kNoPrefix) {
        stack[sp++] = suffix[c];
        c = prefix[c];
      }
      stack[sp++] = suffix[c];
      const uint8_t first_byte = stack[sp - 1];
      for (size_t i = sp; i-- > 0;) out.push_back(stack[i]);
      out.push_back(first_byte);
      if (next_code < kMaxEnt) {
        prefix[next_code] = static_cast<uint16_t>(old);
        suffix[next_code] = first_byte;
        next_code++;
        widthUp();
      }
    } else {
      return -5;  // 非法码（code > next_code）
    }
    old = code;
  }
  return 0;
}

}  // namespace tiff
}  // namespace image_codec
