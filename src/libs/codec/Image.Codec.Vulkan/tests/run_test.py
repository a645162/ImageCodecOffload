#!/usr/bin/env python3
"""Vulkan→GL 零拷贝互操作测试驱动脚本。

用法:
    python run_test.py                    # 配置+构建+运行（build 目录默认 build）
    python run_test.py -b <dir>           # 指定 build 目录
    python run_test.py --skip-build       # 只运行已构建的测试

退出码: 0=PASS, 2=SKIP, 1=FAIL。

File: run_test.py
Brief: Run test.
Author: Haomin Kong
Email: a645162@163.com
Created: 2026-08-12
"""
import argparse
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VCPKG = os.environ.get("VCPKG_ROOT", "C:/vcpkg")


def toolchain():
    tc = os.path.join(VCPKG, "scripts", "buildsystems", "vcpkg.cmake")
    if not os.path.exists(tc):
        print(f"FAIL: 未找到 vcpkg toolchain: {tc}")
        return None
    return tc


def build(build_dir):
    tc = toolchain()
    if tc is None:
        return 1
    cfg = subprocess.run(
        ["cmake", "-S", REPO, "-B", build_dir,
         f"-DCMAKE_TOOLCHAIN_FILE={tc}", "-DVIC_BUILD_TESTS=ON"],
        cwd=REPO)
    if cfg.returncode != 0:
        print("FAIL: CMake 配置失败")
        return 1
    bld = subprocess.run(
        ["cmake", "--build", build_dir, "--config", "Debug",
         "--target", "gl_interop_test"],
        cwd=REPO)
    if bld.returncode != 0:
        print("FAIL: 构建失败")
        return 1
    return 0


def run(build_dir):
    exe = os.path.join(build_dir, "tests", "Debug", "gl_interop_test.exe")
    if not os.path.exists(exe):
        exe = os.path.join(build_dir, "tests", "gl_interop_test.exe")
    if not os.path.exists(exe):
        print(f"FAIL: 测试可执行不存在: {exe}")
        return 1
    r = subprocess.run([exe])
    code = r.returncode
    if code == 0:
        print("PASS")
    elif code == 2:
        print("SKIP: 环境不支持互操作扩展（Vulkan/GL）")
    else:
        # 崩溃/异常码（如 -1073741819）归一化为契约中的 1=FAIL。
        print("FAIL")
        code = 1
    return code


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-b", "--build-dir", default="build")
    ap.add_argument("--skip-build", action="store_true")
    args = ap.parse_args()
    # 统一相对 repo 根解析 build_dir（run()/build() 内部都以 REPO 为准，避免
    # 从非 repo 根目录调用时 build_dir 解析不一致导致的误报 FAIL）。
    if not os.path.isabs(args.build_dir):
        args.build_dir = os.path.join(REPO, args.build_dir)
    if not args.skip_build:
        c = build(args.build_dir)
        if c != 0:
            return c
    return run(args.build_dir)


if __name__ == "__main__":
    sys.exit(main())
