#!/bin/sh
# D4 Object Search demo launcher (runs ON the player, BusyBox sh, as root). Development use only.
#
# Starts a SEPARATE dashboard-server + vm_api_server pair on spare ports (API :20412, dashboard :20398) from binaries and a
# dashboard directory that you copied to $DEV (default /storage/sd/vm-d4-input). The installed extension, its active
# configuration, port 20300 and the live Argus pipeline are not touched; nothing is installed. The object index is written
# to <STORE>/objects.db (visual_memory.db of the store is only read).
#
#   sh vm_object_demo.sh start [STORE_DIR]    default STORE_DIR=/storage/sd/vm-d4-store
#   sh vm_object_demo.sh stop | status
#
# $DEV must contain: vm_api_server dashboard-server dashboard/ (index.html vm.js objects.js ...). Binaries are copied to /tmp
# first because the SD card is mounted noexec. No access token is configured: stop the demo when finished.
EXT=${EXT:-/var/volatile/bsext/ext_npu_argus}
DEV=${DEV:-/storage/sd/vm-d4-input}
BIN=$EXT/RK3588
STORE=${2:-/storage/sd/vm-d4-store}
API_PORT=20412; DASH_PORT=20398
RUN=/tmp/vm-d4-demo
CFG=$RUN/d4.json

pid_of() { ps | grep -F "$1" | grep -v grep | awk 'NR==1{print $1}'; }

case "$1" in
start)
  [ -d "$STORE" ] || { echo "store $STORE does not exist (ingest frames first: see docs/VISUAL_MEMORY_OBJECT_SEARCH.md)"; exit 1; }
  [ -f "$DEV/vm_api_server" ] && [ -f "$DEV/dashboard-server" ] && [ -d "$DEV/dashboard" ] || { echo "missing $DEV/vm_api_server, dashboard-server or dashboard/"; exit 1; }
  mkdir -p $RUN
  cp $DEV/vm_api_server $DEV/dashboard-server $RUN/ && chmod +x $RUN/vm_api_server $RUN/dashboard-server || exit 1
  cat > $CFG <<EOC
{"visual_memory":{"enabled":true,"storage_root":"$STORE","vector_search_enabled":true,"sample_interval_ms":5000,
 "stored_width":640,"stored_height":360,"jpeg_quality":90,"retention_hours":48,
 "embedding":{"enabled":true,"backend":"rknn","model_path":"model/visual_memory/mobilenet_embedder.rknn","retention_days":7},
 "objects":{"enabled":true,"max_regions":20000,"retention_days":30},
 "api":{"npu_core":1,"search_timeout_ms":10000,"idle_unload_s":120}}}
EOC
  [ -n "$(pid_of "$RUN/vm_api_server")" ] && { echo "already running"; exit 0; }
  LD_LIBRARY_PATH=$BIN/lib:$EXT/lib $RUN/vm_api_server --config $CFG --base-dir $BIN --port $API_PORT --viewer > $RUN/api.log 2>&1 &
  MAIN=$(ps | grep '[{]dashboard-serve' | awk 'NR==1{print $1}')
  UP=$( [ -n "$MAIN" ] && tr '\0' ' ' < /proc/$MAIN/cmdline | sed -n 's/.*-upstream \([^ ]*\).*/\1/p')
  UP=${UP:-127.0.0.1:20200}
  $RUN/dashboard-server -port $DASH_PORT -dir $DEV/dashboard -upstream $UP -ws-port 9001 -vm-upstream 127.0.0.1:$API_PORT > $RUN/dash.log 2>&1 &
  sleep 2; echo "object demo started: browse http://<player-ip>:$DASH_PORT/#search?mode=objects  (store $STORE)" ;;
stop)
  for n in "$RUN/vm_api_server" "$RUN/dashboard-server"; do p=$(pid_of "$n"); [ -n "$p" ] && kill $p; done
  sleep 1; echo "object demo stopped" ;;
status)
  echo "api: $(pid_of "$RUN/vm_api_server")  dashboard: $(pid_of "$RUN/dashboard-server")" ;;
*) echo "usage: $0 start [STORE_DIR] | stop | status"; exit 2 ;;
esac
