#!/usr/bin/env bash
# 一键全面测试：v2 协议单测（Py/C++）→ v2 互操作矩阵 → RPC 基准。
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)

echo "## 1/4  Python v2 协议单测（app/sdm/tpc/config/log/watchdog/背压）"
( cd "$ROOT" && python3 -m unittest discover -s tests -p "test_py_*.py" )

echo; echo "## 2/4  C++ v2 编译 + 单测"
( cd "$ROOT" && make -C platform/cpp test )

echo; echo "## 3/4  v2 互操作矩阵（4 组合，约 40s）"
bash "$ROOT/tests/run_interop_matrix.sh"

echo; echo "## 4/4  RPC 基准（延迟/吞吐/丢包退避/背压）"
( cd "$ROOT" && python3 -u benchmarks/bench_rpc.py )

echo; echo "=== ALL CHECKS PASSED ==="