#!/usr/bin/env bash
# Generate a vsomeip.json tuned for the current host.
#
# vsomeip anchors its Service-Discovery multicast group join to the configured
# unicast address. If unicast is 127.0.0.1, the SD socket joins the multicast
# group on loopback while FIND/OFFER packets egress on the real interface, so
# the kernel never delivers them -> "availability never fires". Building the
# config with the host's primary (non-loopback) IPv4 makes in-process SD work.
#
#   ./gen_config.sh [output.json]     (default: ./vsomeip.json)
set -eu

DIR=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-"$DIR/vsomeip.json"}

list_ips() {
    hostname -I 2>/dev/null | tr ' ' '\n' || true
    if command -v ip >/dev/null 2>&1; then
        ip -4 -o addr show scope global 2>/dev/null \
            | awk '{print $4}' | cut -d/ -f1
    fi
}

UNICAST=""
while IFS= read -r ip; do
    case "$ip" in
        127.*|169.254.*) continue ;;
        0.0.0.0)         continue ;;
    esac
    [ -n "$ip" ] || continue
    UNICAST="$ip"
    break
done <<< "$(list_ips)"
[ -n "$UNICAST" ] || UNICAST=127.0.0.1

python3 - "$UNICAST" "$OUT" <<'PY'
import json
import sys

unicast, out = sys.argv[1], sys.argv[2]
cfg = {
    "unicast": unicast,
    "service-discovery": {"enable": True, "multicast": "224.244.224.245"},
    "routing": "someip-service",
    "services": [
        {"service": "0x1234", "instance": "0x5678", "unreliable": "30500"}
    ],
    # App client ports MUST stay distinct from hosted service endpoints:
    # someip-service is the in-process routing manager AND hosts 0x1234 on
    # 30500, so its own client port is 30510; the client app uses 30511,
    # the bench app uses 30512.
    "applications": [
        {"name": "someip-service", "port": "30510"},
        {"name": "someip-client", "port": "30511"},
        {"name": "someip-bench", "port": "30512"}
    ],
    "logging": {"level": "debug", "console": "true"},
}
with open(out, "w") as f:
    json.dump(cfg, f, indent=4)
    f.write("\n")
print("vsomeip.json: unicast=%s -> %s" % (unicast, out))
PY