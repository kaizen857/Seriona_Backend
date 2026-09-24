# CTest 脚本测试：UTF-16 BOM 夹具逐字节比对（F4 的可判负钉子）。
#
# 机制：对 tests/fixtures/lyric_split_utf16 跑 C++ dump，与参考实现当场产出后固化的
# expected.tsv **逐字节**比较，不等即 FATAL_ERROR（⇒ CTest Failed）。
#
# 只用 CMake（${CMAKE_COMMAND} -P），**不依赖 python/xxhsum**：三端 CI 均可运行。
#
# 该用例回归「BOM 预判直达 utf-16」这一修复，且夹具**刻意**含一个判别性载荷：
#   u16le.lrc = `FF FE` + **纯 ASCII** 正文 —— 去掉预判后 ICU 的 Big5 会先接住它
#   （实测 Big5 接受 `FF FE 5B 00 74 00 …`，解出乱码），首行随之变成 big5 且行数变化，
#   与本 expected.tsv 分叉 ⇒ 本目标失败。**判别性来自 ASCII 载荷**：若载荷含 CJK
#   （如 u16le-cjk.lrc），Big5 会拒收，无预判也照样落到 utf-16，用例就失去判别力。
#   u16be.lrc（`FE FF` + ASCII）与 u16le-cjk.lrc 覆盖 BE 序与非 ASCII 载荷的 BOM 剥离。

foreach(variable IN ITEMS DUMP FIXTURE_DIR EXPECTED OUTPUT)
  if(NOT DEFINED ${variable})
    message(FATAL_ERROR "缺少 -D${variable}=…（需 -DDUMP/-DFIXTURE_DIR/-DEXPECTED/-DOUTPUT）")
  endif()
endforeach()

execute_process(
  COMMAND "${DUMP}" --root "${FIXTURE_DIR}" --dump-tsv
  RESULT_VARIABLE dump_rc
  OUTPUT_FILE "${OUTPUT}"
  ERROR_VARIABLE dump_err
)
if(NOT dump_rc EQUAL 0)
  message(FATAL_ERROR "dump 退出码=${dump_rc}（期望 0）；stderr=${dump_err}")
endif()

file(READ "${EXPECTED}" expected_hex HEX)
file(READ "${OUTPUT}" actual_hex HEX)
if(NOT expected_hex STREQUAL actual_hex)
  message(FATAL_ERROR
    "UTF-16 BOM 夹具输出与 expected.tsv 逐字节不一致：expected=${EXPECTED} actual=${OUTPUT}")
endif()

file(READ "${EXPECTED}" expected_text)

if(NOT expected_text MATCHES "total_lines=3 files=3 unresolved_decode=0")
  message(FATAL_ERROR "expected.tsv 的首行统计与夹具不符（期望 total_lines=3 files=3 unresolved_decode=0）")
endif()
if(NOT expected_text MATCHES "decode_by_codec=utf-16=3")
  message(FATAL_ERROR "expected.tsv 未声明 decode_by_codec=utf-16=3（三个文件都应经 utf-16）")
endif()
if(expected_text MATCHES "big5=")
  message(FATAL_ERROR
    "expected.tsv 出现 big5=…：判别性载荷被 ICU Big5 抢先，BOM 预判已失效（F4 回归）")
endif()

message(STATUS "UTF-16 BOM 夹具逐字节一致：decode_by_codec=utf-16=3, total_lines=3, files=3")
