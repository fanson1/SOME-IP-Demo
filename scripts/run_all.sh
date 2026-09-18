#!/usr/bin/env bash
# 一键全面测试：v2 协议单测（Py/C++）→ v1 构建 → v1 互操作矩阵。
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)

echo "## 1/4  Python v2 协议单测（wire/ser，含 v1 golden bytes）"
( cd "$ROOT" && python3 -m unittest discover -s tests -p "test_py_*.py" )

echo; echo "## 2/4  C++ v2 编译 + 单测"
( cd "$ROOT" && make -C platform/cpp test )

echo; echo "## 3/4  C++ v1 构建"
( cd "$ROOT" && make -C platform/cpp all >/dev/null && echo "C++ v1 built OK" )

echo; echo "## 4/4  v1 互操作矩阵（4 组合，约 40s）"
bash "$ROOT/tests/run_interop_matrix.sh"

echo; echo "=== ALL CHECKS PASSED ==="