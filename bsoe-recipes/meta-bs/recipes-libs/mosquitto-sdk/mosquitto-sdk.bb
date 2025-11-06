SUMMARY = "MQTT message broker and client"
DESCRIPTION = "Meta-package to ensure Mosquitto MQTT broker is properly included in SDK for gaze detection telemetry"
LICENSE = "EPL-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/EPL-2.0;md5=84973d94ef4267e37226c1d6d4a5d4f7"

# This is a meta-package for ensuring dependencies
ALLOW_EMPTY:${PN} = "1"

# Runtime dependencies
RDEPENDS:${PN} = " \
    mosquitto \
    mosquitto-clients \
"

# Development packages
RDEPENDS:${PN}-dev = " \
    mosquitto-dev \
"

# Build dependencies
DEPENDS = " \
    mosquitto \
    openssl \
    c-ares \
"
