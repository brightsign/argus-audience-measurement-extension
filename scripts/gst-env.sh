# GStreamer environment setup for RTSP streaming
# Detects platform and sets paths accordingly

# Detect platform based on available SOC directories
if [ -d "/var/volatile/bsext/ext_npu_argus/RK3588" ]; then
  SOC_DIR="/var/volatile/bsext/ext_npu_argus/RK3588"
elif [ -d "/var/volatile/bsext/ext_npu_argus/RK3576" ]; then
  SOC_DIR="/var/volatile/bsext/ext_npu_argus/RK3576"
elif [ -d "/var/volatile/bsext/ext_npu_argus/RK3568" ]; then
  SOC_DIR="/var/volatile/bsext/ext_npu_argus/RK3568"
else
  echo "ERROR: Could not detect SOC platform directory" >&2
  exit 1
fi

# System plugins FIRST for core elements (queue, etc), then bundled plugins
export GST_PLUGIN_PATH=/usr/lib/gstreamer-1.0:${SOC_DIR}/lib/gstreamer-1.0
export LD_LIBRARY_PATH=${SOC_DIR}/lib:${SOC_DIR}/bin:/usr/lib:$LD_LIBRARY_PATH
export GST_REGISTRY="/tmp/gst-registry-$(basename $SOC_DIR).bin"
export GST_REGISTRY_REUSE_PLUGIN_SCANNER=1

echo "GStreamer setup for $(basename $SOC_DIR):"
echo "  GST_PLUGIN_PATH=$GST_PLUGIN_PATH"
echo "  LD_LIBRARY_PATH=$LD_LIBRARY_PATH"

# Note: System plugins provide core elements (queue, typefind, etc)
# Bundled plugins provide RTSP support and hardware decoders
