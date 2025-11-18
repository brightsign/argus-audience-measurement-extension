# Disable git detection to avoid CMake string regex errors in libwebsockets

# Remove any problematic patches first
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# The simplest fix: disable git detection entirely
EXTRA_OECMAKE += "-DGIT_EXECUTABLE:FILEPATH=/bin/false"

# Set reasonable defaults  
EXTRA_OECMAKE += "-DLWS_BUILD_HASH=unknown@localhost-nogit"
