#!/usr/bin/env bash
# v2/v1 互操作矩阵：Python/C++ 两端 4 组合自动化回归。
# 断言以 client 侧输出为准：Discover / GetVersion / Add=7 / Speed 读 88 / Event+notify。
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PYDIR="$ROOT/platform/python"
CPPDIR="$ROOT/platform/cpp"
SLEEP_UP=2
CLIENT_MAX_SECS=40

TMP=$(mktemp -d)
PIDS=()
sweep() {
  pkill -f "$ROOT/platform/python/service_demo.py" 2>/dev/null || true
  pkill -f "$ROOT/platform/python/client_demo.py" 2>/dev/null || true
  pkill -f "$CPPDIR/bin/service_demo" 2>/dev/null || true
  pkill -f "$CPPDIR/bin/client_demo" 2>/dev/null || true
  true
}
cleanup() {
  sweep
  rm -rf "$TMP"
}
trap cleanup EXIT

assert_client() { # name
  local name=$1 f="$TMP/$name.cli.log"
  local ok=1
  grep -q "Discovered service" "$f"        || { ok=0; echo "  FAIL: Discover"; }
  grep -q "GetVersion -> service=" "$f"    || { ok=0; echo "  FAIL: GetVersion"; }
  grep -q "Add(3, 4)" "$f"                 || { ok=0; echo "  FAIL: Add method"; }
  grep -qE "value=88" "$f"                 || { ok=0; echo "  FAIL: Speed=88"; }
  grep -qE "\[event 0x8001\]" "$f"         || { ok=0; echo "  FAIL: event notify"; }
  [ "$ok" -eq 1 ]
  return 0
}

run_case() { # name svc_cmd cli_cmd  (svc/cli 以 PYDIR 为 cwd 执行)
  local name=$1 svc=$2 cli=$3
  sweep
  echo "== $name =="
  ( cd "$PYDIR" && eval "$svc" ) >"$TMP/$name.svc.log" 2>&1 &
  local spid=$!
  sleep "$SLEEP_UP"
  ( cd "$PYDIR" && eval "$cli" ) >"$TMP/$name.cli.log" 2>&1 &
  local cpid=$!
  # watchdog: never let a case hang the whole matrix
  ( sleep "$CLIENT_MAX_SECS"; kill -9 "$cpid" 2>/dev/null ) &
  local wp=$!
  wait "$cpid"
  local crc=$?
  kill "$wp" 2>/dev/null || true
  kill "$spid" 2>/dev/null || true
  sweep
  if [ $crc -ne 0 ]; then
    echo "  FAIL: client exit=$crc"; sed 's/^/    /' "$TMP/$name.cli.log" | tail -5; return 1
  fi
  sed 's/^/    /' "$TMP/$name.cli.log" | grep -E "Discovered|GetVersion|Add\(3|Read Speed|Subscribed|event 0x8001" | head -8
  assert_client "$name" && { echo "  PASS"; return 0; } || { sed -n '1,40p' "$TMP/$name.cli.log" | sed 's/^/    /'; echo "  FAIL: assertions"; return 1; }
}

sweep
sleep 0.5
echo "=== 互操作矩阵（每个组合含 发现/RPC/Field/订阅 断言） ==="
PASS=0; FAIL=0
if run_case py-py "python3 service_demo.py" "python3 client_demo.py";           then PASS=$((PASS+1)); else FAIL=$((FAIL+1)); fi
if run_case cpp-cpp "$CPPDIR/bin/service_demo" "$CPPDIR/bin/client_demo";       then PASS=$((PASS+1)); else FAIL=$((FAIL+1)); fi
if run_case cpp-py "$CPPDIR/bin/service_demo" "python3 client_demo.py";         then PASS=$((PASS+1)); else FAIL=$((FAIL+1)); fi
if run_case py-cpp "python3 service_demo.py" "$CPPDIR/bin/client_demo";         then PASS=$((PASS+1)); else FAIL=$((FAIL+1)); fi

echo "=== 矩阵结果: $PASS passed / $FAIL failed ==="
[ "$FAIL" -eq 0 ]