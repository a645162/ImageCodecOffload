/*
 * File: jpeg_stream.cpp
 * Brief: Jpeg stream.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-10-31
 */

// jpeg_stream.cpp - baseline JPEG 码流分段解析实现。
#include "jpeg_stream.h"

namespace oic {
namespace tiff {

namespace {

constexpr uint8_t kMarkerStart = 0xFF;
constexpr uint8_t kMarkerSoi = 0xD8;
constexpr uint8_t kMarkerEoi = 0xD9;
constexpr uint8_t kMarkerSos = 0xDA;
constexpr uint8_t kMarkerDqt = 0xDB;
constexpr uint8_t kMarkerDht = 0xC4;
constexpr uint8_t kMarkerDri = 0xDD;
constexpr uint8_t kMarkerSof0 = 0xC0;
constexpr uint8_t kMarkerSof1 = 0xC1;
constexpr uint8_t kMarkerSof2 = 0xC2;

bool is_standalone_marker(uint8_t marker) {
  if (marker == kMarkerSoi || marker == 0x01) {
    return true;
  }
  return marker >= 0xD0 && marker <= 0xD7; // RSTn
}

void append_segment(std::vector<uint8_t> &out, const uint8_t *start,
                    size_t length) {
  out.insert(out.end(), start, start + length);
}

// 从 pos 开始扫描熵数据里的 EOI：0xFF 后接 0xD9 才是真 EOI，
// 0xFF00 是字节填充，0xFFD0..D7 是重启标记。
size_t find_eoi(const uint8_t *data, size_t size, size_t pos) {
  while (pos + 1 < size) {
    if (data[pos] != kMarkerStart) {
      ++pos;
      continue;
    }
    const uint8_t next = data[pos + 1];
    if (next == kMarkerEoi) {
      return pos;
    }
    if (next == 0x00 || (next >= 0xD0 && next <= 0xD7)) {
      pos += 2;
      continue;
    }
    ++pos;
  }
  return size;
}

} // namespace

bool parse_jpeg_stream(const uint8_t *data, size_t size, JpegStreamInfo &info,
                       std::string &error) {
  info = JpegStreamInfo{};
  if (data == nullptr || size < 4) {
    error = "JPEG 码流过短";
    return false;
  }
  if (data[0] != kMarkerStart || data[1] != kMarkerSoi) {
    error = "JPEG 码流缺少 SOI";
    return false;
  }

  std::vector<uint8_t> table_segments;
  bool saw_frame = false;
  bool saw_scan = false;

  size_t pos = 2;
  while (pos + 1 < size) {
    if (data[pos] != kMarkerStart) {
      error = "JPEG 段结构损坏（缺少 0xFF 标记前缀）";
      return false;
    }
    size_t marker_pos = pos;
    while (marker_pos + 1 < size && data[marker_pos + 1] == kMarkerStart) {
      ++marker_pos; // 允许填充 0xFF
    }
    const uint8_t marker = data[marker_pos + 1];
    pos = marker_pos + 2;

    if (marker == kMarkerEoi) {
      break;
    }
    if (is_standalone_marker(marker)) {
      continue;
    }
    if (pos + 2 > size) {
      error = "JPEG 段长度越界";
      return false;
    }
    const size_t length = (static_cast<size_t>(data[pos]) << 8) | data[pos + 1];
    if (length < 2 || pos + length > size) {
      error = "JPEG 段长度非法";
      return false;
    }
    const size_t segment_start = marker_pos;
    const size_t segment_length = 2 + length;

    if (marker == kMarkerSof0 || marker == kMarkerSof1 ||
        marker == kMarkerSof2) {
      if (marker != kMarkerSof0) {
        error = "只支持 baseline（SOF0）JPEG，TIFF JPEG 压缩不支持 progressive";
        return false;
      }
      if (length < 8) {
        error = "SOF0 段过短";
        return false;
      }
      // sof 指向段负载（跳过 2 字节长度字段本身）。
      const uint8_t *sof = data + pos + 2;
      info.precision = sof[0];
      info.height = static_cast<uint16_t>((sof[1] << 8) | sof[2]);
      info.width = static_cast<uint16_t>((sof[3] << 8) | sof[4]);
      const uint8_t components = sof[5];
      if (components == 0 || length < static_cast<size_t>(8 + components * 3)) {
        error = "SOF0 分量数非法";
        return false;
      }
      info.components.clear();
      for (uint8_t i = 0; i < components; ++i) {
        JpegComponentInfo component;
        component.id = sof[6 + i * 3];
        component.sampling_h = static_cast<uint8_t>(sof[7 + i * 3] >> 4);
        component.sampling_v = static_cast<uint8_t>(sof[7 + i * 3] & 0x0F);
        component.quant_table = sof[8 + i * 3];
        if (component.sampling_h == 0 || component.sampling_v == 0) {
          error = "SOF0 采样因子为 0";
          return false;
        }
        info.components.push_back(component);
      }
      info.frame_header.clear();
      append_segment(info.frame_header, data + segment_start, segment_length);
      saw_frame = true;
    } else if (marker == kMarkerDqt || marker == kMarkerDht) {
      append_segment(table_segments, data + segment_start, segment_length);
    } else if (marker == kMarkerDri) {
      // DRI 不是「表」，必须留在 strip 里（缩写在 JPEGTables 里的 DRI
      // 不会被所有解码器接受）。
      append_segment(info.frame_header, data + segment_start, segment_length);
    } else if (marker == kMarkerSos) {
      info.scan_header.clear();
      append_segment(info.scan_header, data + segment_start, segment_length);
      const size_t entropy_start = pos + length;
      const size_t eoi = find_eoi(data, size, entropy_start);
      if (eoi >= size) {
        error = "JPEG 码流缺少 EOI";
        return false;
      }
      info.entropy.assign(data + entropy_start, data + eoi);
      saw_scan = true;
      break;
    }
    // 其它段（APPn / COM / DNL …）对 TIFF 无意义，直接丢弃。

    pos += length;
  }

  if (!saw_frame) {
    error = "JPEG 码流缺少 SOF0";
    return false;
  }
  if (!saw_scan) {
    error = "JPEG 码流缺少 SOS";
    return false;
  }
  if (info.width == 0 || info.height == 0) {
    error = "JPEG 宽高为 0";
    return false;
  }
  if (table_segments.empty()) {
    error = "JPEG 码流没有量化/霍夫曼表";
    return false;
  }

  info.tables.clear();
  info.tables.push_back(kMarkerStart);
  info.tables.push_back(kMarkerSoi);
  info.tables.insert(info.tables.end(), table_segments.begin(),
                     table_segments.end());
  info.tables.push_back(kMarkerStart);
  info.tables.push_back(kMarkerEoi);
  return true;
}

bool make_abbreviated_jpeg(const JpegStreamInfo &info,
                           std::vector<uint8_t> &out, std::string &error) {
  out.clear();
  if (info.frame_header.empty() || info.scan_header.empty() ||
      info.entropy.empty()) {
    error = "缩写 JPEG 缺少帧头/扫描头/熵数据";
    return false;
  }
  out.reserve(info.frame_header.size() + info.scan_header.size() +
              info.entropy.size() + 4);
  // 缩写码流仍然以 SOI 开头：libtiff 解码每条 strip 时会先 jpeg_abort，
  // 再从 strip 开头重新读标记；没有 SOI 会被 libjpeg 判成「Not a JPEG
  // file」。表并不在 strip 里，而是来自 JPEGTables（已在 jpeg_abort 之后
  // 仍留在 libjpeg 的永久池里）。
  out.push_back(kMarkerStart);
  out.push_back(kMarkerSoi);
  out.insert(out.end(), info.frame_header.begin(), info.frame_header.end());
  out.insert(out.end(), info.scan_header.begin(), info.scan_header.end());
  out.insert(out.end(), info.entropy.begin(), info.entropy.end());
  out.push_back(kMarkerStart);
  out.push_back(kMarkerEoi);
  return true;
}

} // namespace tiff
} // namespace oic
