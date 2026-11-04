#!/bin/sh
# Visual Memory demo "viewer" (runs ON the player, BusyBox sh, as root).
#
# Starts a second dashboard-server + vm_api_server pair on spare ports that read ONLY a chosen (dataset) store, while
# Argus keeps running on its normal configuration. Port 20300 and the player's active config are not touched. The
# Audience Measurement tab still shows the live camera because the viewer proxies the same image-stream upstream.
#
#   sh vm_demo_viewer.sh start [STORE_DIR]   default STORE_DIR=/storage/sd/vm-demo-dataset
#   sh vm_demo_viewer.sh stop
#   sh vm_demo_viewer.sh status
#
# Browse http://<player>:20399/ (API on loopback :20311). No access token is configured for the viewer: it only exposes the
# dataset store; stop it after the demo. Binaries are copied to /tmp first (the SD card is mounted noexec).
EXT=${EXT:-/var/volatile/bsext/ext_npu_argus}
BIN=$EXT/RK3588
STORE=${2:-/storage/sd/vm-demo-dataset}
API_PORT=20311; DASH_PORT=20399
RUN=/tmp/vm-demo-viewer
CFG=$RUN/viewer.json
PIDS=$RUN/pids

pid_of() { ps | grep -F "$1" | grep -v grep | awk 'NR==1{print $1}'; }

case "$1" in
start)
  [ -d "$STORE" ] || { echo "store $STORE does not exist (build it with vm_ingest_dir first)"; exit 1; }
  mkdir -p $RUN
  cp $BIN/vm_api_server $BIN/dashboard-server $RUN/ && chmod +x $RUN/vm_api_server $RUN/dashboard-server || exit 1
  cat > $CFG <<EOF
{"visual_memory":{"enabled":true,"storage_root":"$STORE","vector_search_enabled":true,"sample_interval_ms":5000,
 "stored_width":640,"stored_height":360,"jpeg_quality":90,"retention_hours":48,
 "embedding":{"enabled":true,"backend":"rknn","model_path":"model/visual_memory/mobilenet_embedder.rknn","retention_days":7},
 "api":{"npu_core":1,"search_timeout_ms":10000,"idle_unload_s":120}}}
EOF
  [ -n "$(pid_of "$RUN/vm_api_server")" ] && { echo "already running"; exit 0; }
  LD_LIBRARY_PATH=$BIN/lib:$EXT/lib $RUN/vm_api_server --config $CFG --base-dir $BIN --port $API_PORT \
      --viewer > $RUN/api.log 2>&1 &
  # reuse the live dashboard's camera upstream so the Audience tab still shows the live stream
  MAIN=$(ps | grep '[{]dashboard-serve' | awk 'NR==1{print $1}')
  UP=$( [ -n "$MAIN" ] && tr '\0' ' ' < /proc/$MAIN/cmdline | sed -n 's/.*-upstream \([^ ]*\).*/\1/p')
  UP=${UP:-127.0.0.1:20200}
  $RUN/dashboard-server -port $DASH_PORT -dir $BIN/dashboard -upstream $UP -ws-port 9001 \
      -vm-upstream 127.0.0.1:$API_PORT > $RUN/dash.log 2>&1 &
  sleep 2; echo "viewer started: browse http://<player-ip>:$DASH_PORT/  (store $STORE)" ;;
stop)
  for n in "$RUN/vm_api_server" "$RUN/dashboard-server"; do
    p=$(pid_of "$n"); [ -n "$p" ] && kill $p
  done
  sleep 1; echo "viewer stopped" ;;
status)
  echo "api: $(pid_of "$RUN/vm_api_server")  dashboard: $(pid_of "$RUN/dashboard-server")" ;;
*) echo "usage: $0 start [STORE_DIR] | stop | status"; exit 2 ;;
esac
