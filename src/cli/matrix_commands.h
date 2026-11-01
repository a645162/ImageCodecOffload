/*
 * File: matrix_commands.h
 * Brief: Matrix subcommand entry points.
 * Author: Haomin Kong
 * Email: a645162@163.com
 * Created: 2026-11-01
 */

// matrix_commands.h - `ico matrix` 与 `ico matrix-metrics` 的入口。
//
// 两条命令的用例不同，但共用「后端 × 格式」的格子枚举：matrix 现场编解码，
// matrix-metrics 只复用既有产物补算指标。把格子枚举放在同一处，是为了保证
// 两份 manifest 的 backend/format 名称与顺序完全一致——否则报告里同名格子
// 对不上号，指标就没法回填到原矩阵。
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "backend.h"

namespace ico_cli {

struct MatrixMetricsOptions final {
  // 只跑这些后端；为空表示全部（cpu / opencl / cuda / vulkan）。
  std::vector<image_codec::offload::BackendKind> backends;
  // 只跑这些格式名（jpeg / tiff / png / jpeg2000-jp2 / ...）；为空表示全部。
  std::vector<std::string> formats;
  // 续跑：读取 report_dir 里已有的 manifest.csv，复用其中 passed/skipped 的行，
  // 只重算 failed 的格子；不写这个开关就是从头跑并覆盖旧 manifest。
  bool resume = false;
  // 指标（MSE/PSNR/SSIM/MS-SSIM）由哪个后端计算；不指定就跟编码产物所属后端
  // 一致。CPU 的 11x11 窗在大图上是分钟级，GPU 是秒级——跑大图矩阵时把它指向
  // 同一块 GPU 即可，解码仍由各产物自己的后端完成（两者语义互不替代）。
  std::optional<image_codec::offload::BackendKind> metrics_backend;
};

int cmdMatrix(const std::string &input, const std::string &output_dir);
int cmdMatrixMetrics(const std::string &input, const std::string &artifacts_dir,
                     const std::string &report_dir,
                     const MatrixMetricsOptions &options);

} // namespace ico_cli
