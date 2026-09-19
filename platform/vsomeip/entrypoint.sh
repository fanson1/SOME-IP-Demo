#!/usr/bin/env bash
# Container entrypoint: regenerate vsomeip.json for the network namespace we are
# running in (needed for in-process SD multicast), then exec the command.
set -eu
cd /app
./gen_config.sh vsomeip.json
export VSOMEIP_CONFIGURATION=/app/vsomeip.json
export LD_LIBRARY_PATH=/usr/local/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
exec "$@"