# File: spv_to_header.cmake
# Brief: Spv to header.
# Author: Haomin Kong
# Email: a645162@163.com
# Created: 2026-10-31

# 将 SPV_FILES(分号分隔的 .spv 路径) 逐个转为 uint32_t 数组写入 OUT_HEADER。
# 生成: static const uint32_t vic_<name>[] = {...}; static const size_t vic_<name>_size = N;
if(NOT DEFINED SPV_FILES OR NOT DEFINED OUT_HEADER)
  message(FATAL_ERROR "usage: -DSPV_FILES=a.spv;b.spv -DOUT_HEADER=x.h -P spv_to_header.cmake")
endif()
file(WRITE ${OUT_HEADER} "#pragma once\n#include <cstdint>\n#include <cstddef>\n")
foreach(spv IN LISTS SPV_FILES)
  get_filename_component(base ${spv} NAME_WE)
  file(READ ${spv} HEXDATA HEX)
  string(LENGTH "${HEXDATA}" LEN)
  if(LEN EQUAL 0)
    message(FATAL_ERROR "empty spv: ${spv}")
  endif()
  string(REGEX MATCHALL ".." BYTES "${HEXDATA}")
  list(LENGTH BYTES NB)
  math(EXPR NWORDS "${NB}/4")
  set(DECL "static const uint32_t vic_${base}[] = {")
  foreach(i RANGE 0 ${NWORDS})
    math(EXPR o "${i}*8")
    string(SUBSTRING "${HEXDATA}" ${o} 8 WORD)
    if(WORD STREQUAL "")
      break()
    endif()
    # SPIR-V 词按小端序存盘：磁盘字节 B0B1 B2B3 B4B5 B6B7 表示词 0xB6B7B4B5B2B3B0B1，
    # 需反转每组 4 字节后形成字面量，否则 vkCreateShaderModule 的 magic 校验失败。
    string(SUBSTRING "${WORD}" 6 2 BYTE3)
    string(SUBSTRING "${WORD}" 4 2 BYTE2)
    string(SUBSTRING "${WORD}" 2 2 BYTE1)
    string(SUBSTRING "${WORD}" 0 2 BYTE0)
    string(APPEND DECL "0x${BYTE3}${BYTE2}${BYTE1}${BYTE0}u,")
  endforeach()
  string(APPEND DECL "};\nstatic const size_t vic_${base}_size = sizeof(vic_${base}) / sizeof(uint32_t);\n")
  file(APPEND ${OUT_HEADER} "${DECL}")
endforeach()
