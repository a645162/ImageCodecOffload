/*
 * File: matrix_commands.cpp
 * Brief: Matrix and matrix-metrics subcommands.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-11-01
 */

// matrix_commands.cpp - `ico matrix` / `ico matrix-metrics` 的实现。
//
// 两条命令都会为「后端 × 格式」的每个格子写一行 manifest。矩阵本身可能要跑
// 数小时（Merge.bmp 单格解码 + 四项指标就有分钟级耗时），所以矩阵指标命令
// 走**逐格原子 checkpoint**：每算完一格就把整份 manifest 重写到 <name>.tmp
// 再改名覆盖。进程被 Ctrl-C 或 OOM 杀掉时，磁盘上留下的一定是「上一次完整
// 提交」的 manifest，绝不会是半行 CSV；`--resume` 据此复用已完成的格子。
//
// 为什么不是「追加一行 + flush」：追加能在中途留下半行（写了一半被杀），
// 续跑时该行既不能解析也不能丢弃——重算它就等于把「已完成」变成「不确定」。
// 32 行的整份重写代价可以忽略，换来的是续跑语义没有歧义。
#include "matrix_commands.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "Image/Codec/Offload/image_codec_offload.h"
#include "cli_common.h"
#include "image_codec_types.h"
#include "metrics.h"
#include "router.h"

namespace ico_cli {

namespace {

using image_codec::offload::BackendKind;
using image_codec::offload::CodecDirection;
using image_codec::offload::EncodeParams;
using image_codec::offload::Format;
using image_codec::offload::Image;
using image_codec::offload::Jpeg2000Container;
using image_codec::offload::Router;

struct MatrixFormat final {
  Format format;
  const char *name;
  const char *suffix;
  Jpeg2000Container jpeg2000_container = Jpeg2000Container::Jp2;
};

// JPEG 2000 的四种用户可选后缀不能在矩阵中折叠成一个抽象 Format：.j2k
// 是裸 codestream，而 .jp2/.jpf/.jpx 都必须实际产出并重新解码验证。
const std::vector<MatrixFormat> &matrixFormats() {
  static const std::vector<MatrixFormat> formats = {
      {Format::Jpeg, "jpeg", "jpg"},
      {Format::Tiff, "tiff", "tiff"},
      {Format::Png, "png", "png"},
      {Format::Jpeg2000, "jpeg2000-jp2", "jp2"},
      {Format::Jpeg2000, "jpeg2000-j2k", "j2k", Jpeg2000Container::J2k},
      {Format::Jpeg2000, "jpeg2000-jpf", "jpf"},
      {Format::Jpeg2000, "jpeg2000-jpx", "jpx"},
      {Format::JpegXl, "jpeg-xl", "jxl"},
  };
  return formats;
}

const std::vector<BackendKind> &matrixBackends() {
  static const std::vector<BackendKind> backends = {
      BackendKind::Cpu, BackendKind::OpenCl, BackendKind::Cuda,
      BackendKind::Vulkan};
  return backends;
}

constexpr const char *kMatrixHeader =
    "backend,format,status,encode_mode,decode_mode,bytes,width,height,"
    "channels,codec_elapsed_ms,metrics_status,mse,psnr,ssim,"
    "ms_ssim,metrics_elapsed_ms,host_rss_peak_mb,device_peak_mb,error\n";

constexpr const char *kMetricsHeader =
    "backend,format,status,encode_mode,decode_mode,bytes,width,height,"
    "channels,decode_elapsed_ms,metrics_backend,metrics_status,mse,psnr,ssim,"
    "ms_ssim,metrics_elapsed_ms,error\n";

// 指标错误里只有「这台机器/这个后端做不到」才允许记 skipped；
// 其它一律 failed，否则真实缺陷会被一行 skipped 掩盖。
template <typename Error> bool isSkippableMetricError(const Error &error) {
  return error.code == image_codec::offload::Status::BackendUnavailable ||
         error.code == image_codec::offload::Status::UnsupportedFormat ||
         error.code == image_codec::offload::Status::OutOfMemory;
}

// 逐格原子 checkpoint：行按本轮「后端 × 格式」的枚举顺序落盘，续跑重算的格子
// 会回到原位，因此「一次跑完」与「中断后续跑」得到的 manifest 逐行相同。
class ManifestCheckpoint final {
public:
  bool open(const std::filesystem::path &path, std::string header,
            const bool resume, std::string *err) {
    path_ = path;
    header_ = std::move(header);
    if (resume && std::filesystem::exists(path_)) {
      if (!loadCompleted(path_, err)) return false;
    }
    return flush(err);
  }

  bool contains(const std::string &key) const {
    return rows_.find(key) != rows_.end();
  }

  // 续跑复用的格子要按它**原来的结论**计数，不能一律记成 passed：
  // 上一轮因缺产物而 skipped 的格子，这一轮并没有变成成功。
  std::string statusOf(const std::string &key) const {
    const auto found = rows_.find(key);
    return found == rows_.end() ? std::string() : found->second.status;
  }

  // 这一格上一轮的指标是在哪个后端算的。换了 --metrics-backend 之后不能直接
  // 复用：同一份 manifest 里混进两种设备的数字，横向比较就失去意义。
  std::string metricsBackendOf(const std::string &key) const {
    const auto found = rows_.find(key);
    return found == rows_.end() ? std::string() : found->second.metrics_backend;
  }

  // 本轮的格子顺序（后端 × 格式的枚举顺序）。续跑重算的格子按这个顺序落回
  // 原位，于是「一次跑完」与「中断后续跑」得到逐行相同的 manifest。
  void setOrder(std::vector<std::string> order) {
    order_ = std::move(order);
    order_set_.clear();
    order_set_.insert(order_.begin(), order_.end());
  }

  void setRow(const std::string &key, std::string row, std::string status,
              std::string metrics_backend) {
    if (rows_.find(key) == rows_.end()) leftover_.push_back(key);
    rows_[key] =
        Entry{std::move(row), std::move(status), std::move(metrics_backend)};
  }

  bool flush(std::string *err) {
    const std::filesystem::path temporary = path_.string() + ".tmp";
    std::ofstream out(temporary, std::ios::trunc | std::ios::binary);
    if (!out) {
      *err = "cannot create " + temporary.string();
      return false;
    }
    out << header_;
    for (const std::string &key : order_) {
      const auto found = rows_.find(key);
      if (found != rows_.end()) out << found->second.row;
    }
    // 上一轮跑过、这一轮被过滤掉的格子：保留原文，免得 `--only-format png
    // --resume` 把已有的 jpeg 结果从报告里抹掉。它们排在选定格子之后。
    for (const std::string &key : leftover_) {
      if (order_set_.find(key) != order_set_.end()) continue;
      const auto found = rows_.find(key);
      if (found != rows_.end()) out << found->second.row;
    }
    out.close();
    if (!out) {
      *err = "cannot write " + temporary.string();
      return false;
    }
    std::error_code rename_error;
    std::filesystem::rename(temporary, path_, rename_error);
    if (rename_error) {
      // Windows 的 rename 不允许覆盖已存在的文件；先删再改名。
      std::error_code remove_error;
      std::filesystem::remove(path_, remove_error);
      rename_error.clear();
      std::filesystem::rename(temporary, path_, rename_error);
      if (rename_error) {
        *err =
            "cannot replace " + path_.string() + ": " + rename_error.message();
        return false;
      }
    }
    return true;
  }

private:
  struct Entry final {
    std::string row;
    std::string status;
    std::string metrics_backend;
  };

  // 续跑只复用「已有结论」的格子：passed / skipped 不会再变，failed 则可能
  // 只是被中断或临时资源不足，重算才是用户想要的。字段 0..10 由本程序写入，
  // 不含引号与逗号，可以按位置切分（error 是最后一个字段，这里用不到）。
  bool loadCompleted(const std::filesystem::path &path, std::string *err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      *err = "cannot read " + path.string();
      return false;
    }
    std::string line;
    std::getline(in, line); // 表头
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      std::string fields[11];
      std::size_t pos = 0;
      bool complete = true;
      for (int index = 0; index < 11; ++index) {
        const std::size_t comma = line.find(',', pos);
        if (comma == std::string::npos) {
          complete = false;
          break;
        }
        fields[index] = line.substr(pos, comma - pos);
        pos = comma + 1;
      }
      if (!complete) continue;
      const std::string &status = fields[2];
      if (status != "passed" && status != "skipped") continue;
      setRow(fields[0] + "/" + fields[1], line + "\n", status, fields[10]);
    }
    return true;
  }

  std::filesystem::path path_;
  std::string header_;
  std::vector<std::string> order_;
  std::vector<std::string> leftover_;
  std::set<std::string> order_set_;
  std::map<std::string, Entry> rows_;
};

std::string cellKey(const std::string &backend, const std::string &format) {
  return backend + "/" + format;
}

// manifest 里的数值列必须是 17 位有效数字：MSE 常在 1e-6 量级，默认 6 位
// 会把不同后端的差异抹平，回填报告时又变成"看起来完全一致"。
std::ostringstream makeRow() {
  std::ostringstream row;
  row << std::setprecision(17);
  return row;
}

// 过滤后的格子集合；顺序沿用 matrixBackends/matrixFormats，保证 manifest 稳定。
std::vector<BackendKind>
selectedBackends(const std::vector<BackendKind> &requested) {
  if (requested.empty()) return matrixBackends();
  std::vector<BackendKind> selected;
  for (const BackendKind backend : matrixBackends()) {
    if (std::find(requested.begin(), requested.end(), backend) !=
        requested.end()) {
      selected.push_back(backend);
    }
  }
  return selected;
}

std::vector<const MatrixFormat *>
selectedFormats(const std::vector<std::string> &requested) {
  std::vector<const MatrixFormat *> selected;
  for (const MatrixFormat &format : matrixFormats()) {
    if (requested.empty() || std::find(requested.begin(), requested.end(),
                                       format.name) != requested.end()) {
      selected.push_back(&format);
    }
  }
  return selected;
}

// 拼错的格式名要让调用方**立刻**看到可用名字，所以这份清单全部走 stderr：
// stdout 是矩阵进度与汇总，错误信息混进去会让人以为跑了一格。
void printUnknownFormats(const std::vector<std::string> &requested) {
  for (const std::string &name : requested) {
    std::fprintf(stderr, "unknown format filter: %s\n", name.c_str());
  }
  std::fprintf(stderr, "available formats:");
  for (const MatrixFormat &format : matrixFormats()) {
    std::fprintf(stderr, " %s", format.name);
  }
  std::fprintf(stderr, "\n");
}

// manifest 里的资源列：没测到就写**空字段**，不能写 0。0 在报告里读作
// 「这一格没占内存」，与「这个后端不提供计数」是完全不同的结论。
std::string megaBytesOrEmpty(const uint64_t bytes) {
  if (bytes == 0) return {};
  std::ostringstream text;
  text << std::setprecision(17) << (static_cast<double>(bytes) / (1024.0 * 1024.0));
  return text.str();
}

} // namespace

int cmdMatrix(const std::string &input, const std::string &output_dir,
              const MatrixFilter &filter) {
  // 与 matrix-metrics 同样的理由：拼错的格式名必须在读图之前报错，绝不能
  // 退化成「跑全部」——那会先把一张 1.9 GB 的图读进内存再报错。
  std::vector<std::string> unknown_formats;
  for (const std::string &name : filter.formats) {
    const bool known = std::any_of(
        matrixFormats().begin(), matrixFormats().end(),
        [&name](const MatrixFormat &format) { return name == format.name; });
    if (!known) unknown_formats.push_back(name);
  }
  if (!unknown_formats.empty()) {
    printUnknownFormats(unknown_formats);
    std::fprintf(stderr, "error: --only-format matched no matrix format\n");
    return 1;
  }

  Router router;
  std::string error;
  Image source;
  if (!loadImage(router, input, &source, &error)) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return 1;
  }

  std::error_code directory_error;
  std::filesystem::create_directories(output_dir, directory_error);
  if (directory_error) {
    std::fprintf(stderr, "error: cannot create matrix directory: %s\n",
                 directory_error.message().c_str());
    return 1;
  }
  std::ofstream manifest(std::filesystem::path(output_dir) / "manifest.csv",
                         std::ios::trunc);
  if (!manifest) {
    std::fprintf(stderr, "error: cannot create matrix manifest\n");
    return 1;
  }
  manifest << kMatrixHeader << std::setprecision(17);

  bool hard_failure = false;
  for (const BackendKind backend : selectedBackends(filter.backends)) {
    for (const MatrixFormat *matrix_format :
         selectedFormats(filter.formats)) {
      const std::string backend_name =
          image_codec::offload::backendName(backend);
      const std::string format_name = matrix_format->name;
      const std::string encode_mode =
          image_codec::offload::executionModeName(router.executionMode(
              backend, matrix_format->format, CodecDirection::Encode));
      const std::string decode_mode =
          image_codec::offload::executionModeName(router.executionMode(
              backend, matrix_format->format, CodecDirection::Decode));
      const auto output =
          std::filesystem::path(output_dir) /
          (backend_name + "-" + format_name + "." + matrix_format->suffix);
      // 资源峰值必须**在格子内**采样：1.9 GB 的帧峰值出现在编码过程中
      // （源 + 中转 + 产物同时在手），格子结束再看一眼只会看到已经回落的数字。
      // device_peak_mb 取后端句柄自报的历史峰值：CUDA 真实统计、Vulkan 恒 0
      // （句柄不持显存）、OpenCL 不提供——后两者写空字段而不是 0。
      HostPeakSampler host_sampler;
      host_sampler.start();
      bool resources_done = false;
      std::string resource_fields;
      const auto finish_resources = [&]() -> const std::string & {
        if (!resources_done) {
          resources_done = true;
          uint64_t device_peak = 0;
          if (const auto usage = router.memoryUsage(backend); usage) {
            device_peak = usage->peak_bytes;
          }
          resource_fields = "," + megaBytesOrEmpty(host_sampler.stop()) + "," +
                            megaBytesOrEmpty(device_peak);
        }
        return resource_fields;
      };

      const long long start = nowMicros();
      EncodeParams params;
      params.jpeg2000_container = matrix_format->jpeg2000_container;
      auto encoded =
          router.encodeWith(backend, matrix_format->format, source, params);
      const double elapsed = static_cast<double>(nowMicros() - start) / 1000.0;
      if (!encoded) {
        const auto status = encoded.error().code;
        const std::string escaped = encoded.error().message;
        const bool skipped =
            status == image_codec::offload::Status::BackendUnavailable ||
            status == image_codec::offload::Status::UnsupportedFormat ||
            status == image_codec::offload::Status::OutOfMemory;
        manifest << backend_name << ',' << format_name << ','
                 << (skipped ? "skipped" : "failed") << ',' << encode_mode
                 << ',' << decode_mode << ",0,0,0,0," << elapsed
                 << ",not-run,,,,,0" << finish_resources() << ','
                 << csvField(escaped) << '\n';
        // 大图矩阵一跑就是几十分钟，中途被终止时已完成的格子必须留在盘上。
        manifest.flush();
        if (skipped) {
          std::printf("matrix %-6s %-12s skipped (encode=%s, decode=%s): %s\n",
                      backend_name.c_str(), format_name.c_str(),
                      encode_mode.c_str(), decode_mode.c_str(),
                      escaped.c_str());
        } else {
          hard_failure = true;
        }
        continue;
      }
      if (!writeFile(output.string(), *encoded, &error)) {
        manifest << backend_name << ',' << format_name << ",failed,"
                 << encode_mode << ',' << decode_mode << ",0,0,0,0," << elapsed
                 << ",not-run,,,,,0" << finish_resources() << ','
                 << csvField(error) << '\n';
        manifest.flush();
        hard_failure = true;
        continue;
      }
      auto decoded = router.decodeWith(backend, matrix_format->format,
                                       encoded->data(), encoded->size());
      if (!decoded || decoded->width != source.width ||
          decoded->height != source.height ||
          decoded->channels != source.channels) {
        const std::string message = decoded
                                        ? "decoded geometry/channel mismatch"
                                        : decoded.error().message;
        manifest << backend_name << ',' << format_name << ",failed,"
                 << encode_mode << ',' << decode_mode << ',' << encoded->size()
                 << ",0,0,0," << elapsed << ',' << "not-run,,,,,0"
                 << finish_resources() << ',' << csvField(message) << '\n';
        manifest.flush();
        hard_failure = true;
        continue;
      }
      const long long metrics_start = nowMicros();
      auto mse = router.mseWith(backend, source, *decoded);
      auto ssim = router.ssimWith(backend, source, *decoded);
      const double metrics_elapsed =
          static_cast<double>(nowMicros() - metrics_start) / 1000.0;
      const bool metrics_ok = mse && ssim;
      std::string metrics_error;
      if (!mse) {
        metrics_error = mse.error().message;
      }
      if (!ssim) {
        if (!metrics_error.empty()) metrics_error += "; ";
        metrics_error += ssim.error().message;
      }
      const bool metrics_hard_failure =
          (!mse && !isSkippableMetricError(mse.error())) ||
          (!ssim && !isSkippableMetricError(ssim.error()));
      const bool metrics_skipped = !metrics_ok && !metrics_hard_failure;
      const char *metrics_status =
          metrics_ok ? "passed" : (metrics_skipped ? "skipped" : "failed");
      manifest << backend_name << ',' << format_name << ",passed,"
               << encode_mode << ',' << decode_mode << ',' << encoded->size()
               << ',' << decoded->width << ',' << decoded->height << ','
               << decoded->channels << ',' << elapsed << ',' << metrics_status
               << ',';
      if (metrics_ok) {
        manifest << *mse << ','
                 << image_codec::offload::metrics::psnrFromMse(*mse) << ','
                 << ssim->ssim << ',' << ssim->ms_ssim;
      } else {
        manifest << ",,,";
        if (!metrics_skipped) hard_failure = true;
      }
      manifest << ',' << metrics_elapsed << finish_resources() << ','
               << csvField(metrics_error) << '\n';
      manifest.flush();
      if (metrics_ok) {
        std::printf("matrix %-6s %-12s passed (encode=%s, decode=%s, mse=%.9f, "
                    "psnr=%.9f, ssim=%.9f, ms-ssim=%.9f, %.1f ms)\n",
                    backend_name.c_str(), format_name.c_str(),
                    encode_mode.c_str(), decode_mode.c_str(), *mse,
                    image_codec::offload::metrics::psnrFromMse(*mse),
                    ssim->ssim, ssim->ms_ssim, elapsed + metrics_elapsed);
      } else {
        std::printf("matrix %-6s %-12s codec=passed metrics=%s: %s\n",
                    backend_name.c_str(), format_name.c_str(), metrics_status,
                    metrics_error.c_str());
      }
    }
  }
  return hard_failure ? 1 : 0;
}

int cmdMatrixMetrics(const std::string &input, const std::string &artifacts_dir,
                     const std::string &report_dir,
                     const MatrixMetricsOptions &options) {
  // 参数校验必须在读图之前：拼错的格式名绝不能退化成「跑全部」——那不只是
  // 「以为只跑了一格」的误会，还会先把一张 1.9 GB 的图读进内存再报错。
  std::vector<std::string> unknown_formats;
  for (const std::string &name : options.filter.formats) {
    const bool known = std::any_of(
        matrixFormats().begin(), matrixFormats().end(),
        [&name](const MatrixFormat &format) { return name == format.name; });
    if (!known) unknown_formats.push_back(name);
  }
  if (!unknown_formats.empty()) {
    printUnknownFormats(unknown_formats);
    std::fprintf(stderr, "error: --only-format matched no matrix format\n");
    return 1;
  }

  Router router;
  std::string error;
  Image source;
  if (!loadImage(router, input, &source, &error)) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return 1;
  }

  const std::vector<BackendKind> backends =
      selectedBackends(options.filter.backends);
  const std::vector<const MatrixFormat *> formats =
      selectedFormats(options.filter.formats);

  std::error_code directory_error;
  std::filesystem::create_directories(report_dir, directory_error);
  if (directory_error) {
    std::fprintf(stderr, "error: cannot create metrics report directory: %s\n",
                 directory_error.message().c_str());
    return 1;
  }

  const std::filesystem::path manifest_path =
      std::filesystem::path(report_dir) / "manifest.csv";
  ManifestCheckpoint checkpoint;
  if (!checkpoint.open(manifest_path, kMetricsHeader, options.resume, &error)) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return 1;
  }
  // 行顺序由本轮的格子枚举决定，不受「哪一格先算完」影响。
  std::vector<std::string> cell_order;
  cell_order.reserve(backends.size() * formats.size());
  for (const BackendKind backend : backends) {
    for (const MatrixFormat *matrix_format : formats) {
      cell_order.push_back(cellKey(image_codec::offload::backendName(backend),
                                   matrix_format->name));
    }
  }
  checkpoint.setOrder(cell_order);
  if (options.resume) {
    std::printf("matrix-metrics resume: %s\n", manifest_path.string().c_str());
  }

  const std::size_t total = backends.size() * formats.size();
  std::size_t index = 0;
  std::size_t passed = 0;
  std::size_t skipped = 0;
  std::size_t failed = 0;
  std::size_t reused = 0;
  bool hard_failure = false;

  for (const BackendKind backend : backends) {
    for (const MatrixFormat *matrix_format : formats) {
      ++index;
      const std::string backend_name =
          image_codec::offload::backendName(backend);
      const std::string format_name = matrix_format->name;
      const std::string key = cellKey(backend_name, format_name);
      const std::string prefix = "[" + std::to_string(index) + "/" +
                                 std::to_string(total) + "] " + backend_name +
                                 " " + format_name;
      const BackendKind metrics_backend =
          options.metrics_backend.value_or(backend);
      const std::string metrics_backend_name =
          image_codec::offload::backendName(metrics_backend);
      // 复用条件包含「指标后端没变」：换了设备就算没完成，重算并覆盖，
      // 绝不让同一份 manifest 里混进两种设备算出来的数字。
      if (checkpoint.contains(key) &&
          checkpoint.metricsBackendOf(key) == metrics_backend_name) {
        ++reused;
        const std::string prior = checkpoint.statusOf(key);
        if (prior == "skipped") {
          ++skipped;
        } else {
          ++passed;
        }
        std::printf("%s: reused completed cell (%s)\n", prefix.c_str(),
                    prior.c_str());
        continue;
      }
      const std::string encode_mode =
          image_codec::offload::executionModeName(router.executionMode(
              backend, matrix_format->format, CodecDirection::Encode));
      const std::string decode_mode =
          image_codec::offload::executionModeName(router.executionMode(
              backend, matrix_format->format, CodecDirection::Decode));
      const auto artifact =
          std::filesystem::path(artifacts_dir) /
          (backend_name + "-" + format_name + "." + matrix_format->suffix);
      std::error_code exists_error;
      const bool exists = std::filesystem::exists(artifact, exists_error);
      const auto commit = [&](std::string row, const char *status,
                              const std::string &reason) {
        checkpoint.setRow(key, std::move(row), status, metrics_backend_name);
        std::string write_error;
        if (!checkpoint.flush(&write_error)) {
          std::fprintf(stderr, "error: %s\n", write_error.c_str());
          hard_failure = true;
          return false;
        }
        std::printf("%s: %s%s%s\n", prefix.c_str(), status,
                    reason.empty() ? "" : " - ", reason.c_str());
        return true;
      };
      if (!exists) {
        const std::string reason =
            exists_error ? exists_error.message() : "encoded artifact missing";
        auto row = makeRow();
        row << backend_name << ',' << format_name
            << (exists_error ? ",failed," : ",skipped,") << encode_mode << ','
            << decode_mode << ",0,0,0,0,0," << metrics_backend_name
            << ",not-run,,,,,0," << csvField(reason) << '\n';
        if (exists_error) {
          ++failed;
          hard_failure = true;
        } else {
          ++skipped;
        }
        if (!commit(row.str(), exists_error ? "failed" : "skipped", reason)) {
          return 1;
        }
        continue;
      }

      std::vector<uint8_t> encoded;
      if (!readFile(artifact.string(), &encoded, &error)) {
        auto row = makeRow();
        row << backend_name << ',' << format_name << ",failed," << encode_mode
            << ',' << decode_mode << ",0,0,0,0,0," << metrics_backend_name
            << ",not-run,,,,,0," << csvField(error) << '\n';
        ++failed;
        hard_failure = true;
        std::fprintf(stderr, "error: %s\n", error.c_str());
        if (!commit(row.str(), "failed", error)) return 1;
        continue;
      }
      const long long decode_start = nowMicros();
      auto decoded = router.decodeWith(backend, matrix_format->format,
                                       encoded.data(), encoded.size());
      const double decode_elapsed =
          static_cast<double>(nowMicros() - decode_start) / 1000.0;
      if (!decoded) {
        const auto status = decoded.error().code;
        const bool cell_skipped =
            status == image_codec::offload::Status::BackendUnavailable ||
            status == image_codec::offload::Status::UnsupportedFormat ||
            status == image_codec::offload::Status::OutOfMemory;
        auto row = makeRow();
        row << backend_name << ',' << format_name << ','
            << (cell_skipped ? "skipped" : "failed") << ',' << encode_mode
            << ',' << decode_mode << ',' << encoded.size() << ",0,0,0,"
            << decode_elapsed << ',' << metrics_backend_name
            << ",not-run,,,,,0," << csvField(decoded.error().message) << '\n';
        if (cell_skipped) {
          ++skipped;
        } else {
          ++failed;
          hard_failure = true;
        }
        if (!commit(row.str(), cell_skipped ? "skipped" : "failed",
                    decoded.error().message)) {
          return 1;
        }
        continue;
      }
      if (decoded->width != source.width || decoded->height != source.height ||
          decoded->channels != source.channels) {
        const std::string reason = "decoded geometry/channel mismatch";
        auto row = makeRow();
        row << backend_name << ',' << format_name << ",failed," << encode_mode
            << ',' << decode_mode << ',' << encoded.size() << ','
            << decoded->width << ',' << decoded->height << ','
            << decoded->channels << ',' << decode_elapsed << ','
            << metrics_backend_name << ",not-run,,,,,0," << csvField(reason)
            << '\n';
        ++failed;
        hard_failure = true;
        if (!commit(row.str(), "failed", reason)) return 1;
        continue;
      }

      // 指标后端与解码/编码后端刻意分开：解码必须用产物自己的后端（否则测的
      // 不是这个格子），而 11x11 窗在大图上 CPU 是分钟级、GPU 是秒级，允许换。
      const long long metrics_start = nowMicros();
      auto mse = router.mseWith(metrics_backend, source, *decoded);
      auto ssim = router.ssimWith(metrics_backend, source, *decoded);
      const double metrics_elapsed =
          static_cast<double>(nowMicros() - metrics_start) / 1000.0;
      std::string metrics_error;
      // 两个指标经常同时失败（同一个后端不可用），把重复的原因合并成一条，
      // 免得 manifest 里出现「A; A」这种既长又看不出差别的错误文本。
      if (!mse) metrics_error = mse.error().message;
      if (!ssim) {
        const std::string &ssim_error = ssim.error().message;
        if (metrics_error.empty()) {
          metrics_error = ssim_error;
        } else if (metrics_error != ssim_error) {
          metrics_error += "; ";
          metrics_error += ssim_error;
        }
      }
      const bool metrics_ok = mse && ssim;
      const bool metrics_hard_failure =
          (!mse && !isSkippableMetricError(mse.error())) ||
          (!ssim && !isSkippableMetricError(ssim.error()));
      const bool metrics_skipped = !metrics_ok && !metrics_hard_failure;
      const char *metrics_status =
          metrics_ok ? "passed" : (metrics_skipped ? "skipped" : "failed");
      auto row = makeRow();
      row << backend_name << ',' << format_name << ','
          << (metrics_hard_failure ? "failed" : "passed") << ',' << encode_mode
          << ',' << decode_mode << ',' << encoded.size() << ','
          << decoded->width << ',' << decoded->height << ','
          << decoded->channels << ',' << decode_elapsed << ','
          << metrics_backend_name << ',' << metrics_status << ',';
      if (metrics_ok) {
        row << *mse << ',' << image_codec::offload::metrics::psnrFromMse(*mse)
            << ',' << ssim->ssim << ',' << ssim->ms_ssim;
      } else {
        row << ",,,";
      }
      row << ',' << metrics_elapsed << ',' << csvField(metrics_error) << '\n';
      if (metrics_hard_failure) {
        ++failed;
        hard_failure = true;
      } else if (metrics_ok) {
        ++passed;
      } else {
        ++skipped;
      }
      if (!commit(row.str(), metrics_status,
                  metrics_ok ? std::string() : metrics_error)) {
        return 1;
      }
    }
  }

  std::printf("matrix-metrics: %zu cells (passed=%zu, skipped=%zu, failed=%zu, "
              "reused=%zu)\n",
              total, passed, skipped, failed, reused);
  return hard_failure ? 1 : 0;
}

} // namespace ico_cli
