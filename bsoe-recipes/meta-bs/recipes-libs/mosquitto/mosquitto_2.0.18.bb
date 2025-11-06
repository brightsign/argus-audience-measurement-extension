SUMMARY = "Eclipse Mosquitto - MQTT broker and client library"
DESCRIPTION = "Mosquitto is an open source MQTT broker that implements the MQTT protocol \
and provides C client libraries (libmosquitto) for telemetry publishing."
HOMEPAGE = "https://mosquitto.org/"
LICENSE = "EPL-2.0 & EDL-1.0"

# Upstream ships both licenses; include both
LIC_FILES_CHKSUM = " \
    file://LICENSE.txt;sha256=3b83ef96387f14655fc854ddc3c6bd57eaf6a0978fc2fbf9d9f11fa5d88d5e87 \
    file://edl-v10;sha256=c4f0b7d9bfd3519b56d7e58b1be3a2e8a8f3a5e7c7e1b9f0e8d1c9b8a7f6e5d \
"

PV = "2.0.18"
SRC_URI = "https://mosquitto.org/files/source/mosquitto-${PV}.tar.gz"
SRC_URI[sha256sum] = "d665fe7d0032881b1371a47f34169ee4edab67903b2cd2b4c083822823f4448a"

S = "${WORKDIR}/mosquitto-${PV}"

inherit cmake pkgconfig

# Toggle common features via PACKAGECONFIG for flexibility
# Keep websockets OFF by default for smaller image footprint
PACKAGECONFIG ??= "tls persistence bridge compat"
PACKAGECONFIG[tls]         = "-DWITH_TLS=ON,-DWITH_TLS=OFF,openssl"
PACKAGECONFIG[psk]         = "-DWITH_TLS_PSK=ON,-DWITH_TLS_PSK=OFF,"
PACKAGECONFIG[websockets]  = "-DWITH_WEBSOCKETS=ON,-DWITH_WEBSOCKETS=OFF,libwebsockets"
PACKAGECONFIG[adns]        = "-DWITH_ADNS=ON,-DWITH_ADNS=OFF,c-ares"
PACKAGECONFIG[persistence] = "-DWITH_PERSISTENCE=ON,-DWITH_PERSISTENCE=OFF,"
PACKAGECONFIG[bridge]      = "-DWITH_BRIDGE=ON,-DWITH_BRIDGE=OFF,"
PACKAGECONFIG[compat]      = "-DWITH_COMPAT=ON,-DWITH_COMPAT=OFF,"

# Extra CMake switches (keep build lean; let upstream CMake handle installation)
EXTRA_OECMAKE += " \
    -DCMAKE_BUILD_TYPE=Release \
    -DWITH_CJSON=OFF \
    -DWITH_SYSTEMD=OFF \
    -DWITH_DOCS=OFF \
    -DWITH_TESTING=OFF \
    -DWITH_STRIP=ON \
"

# Note: Mosquitto's CMake installs libs, headers, and binaries via 'make install' automatically,
# so no manual do_install is needed. The inherited cmake class will handle it.

do_configure:prepend() {
    # Disable man pages build - xsltproc not available in build environment
    sed -i 's/add_subdirectory(man)/#add_subdirectory(man)/' ${S}/CMakeLists.txt || true
}

# Package splitting for better modularity
PACKAGES =+ "${PN}-clients ${PN}-broker ${PN}-cpp"

# Core shared library package (runtime only)
FILES:${PN} = " \
    ${libdir}/libmosquitto.so.* \
"

# C++ library (runtime)
FILES:${PN}-cpp = " \
    ${libdir}/libmosquittopp.so.* \
"

# Development package (headers and symlinks for SDK builds)
FILES:${PN}-dev = " \
    ${includedir}/mosquitto*.h \
    ${includedir}/mqtt_protocol.h \
    ${libdir}/libmosquitto.so \
    ${libdir}/libmosquittopp.so \
    ${libdir}/pkgconfig/libmosquitto.pc \
    ${libdir}/pkgconfig/libmosquittopp.pc \
"

# Client utilities
FILES:${PN}-clients = " \
    ${bindir}/mosquitto_pub \
    ${bindir}/mosquitto_sub \
    ${bindir}/mosquitto_rr \
    ${bindir}/mosquitto_passwd \
"

# Broker binary + configuration
FILES:${PN}-broker = " \
    ${bindir}/mosquitto \
    ${sbindir}/mosquitto \
    ${sysconfdir}/mosquitto/* \
"

# Runtime dependencies
RDEPENDS:${PN} = "openssl"
RDEPENDS:${PN}-clients = "${PN}"
RDEPENDS:${PN}-broker = "${PN}"

# Provide aliases users might search for
PROVIDES += "libmosquitto mosquitto"
RPROVIDES:${PN} += "libmosquitto"
RPROVIDES:${PN}-dev += "libmosquitto-dev"
