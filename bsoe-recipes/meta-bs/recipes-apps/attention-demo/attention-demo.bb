SUMMARY = "BrightSign Gaze/Attention Demo - Real-time face detection and gaze tracking"
DESCRIPTION = "Attention demo application for XT5 that performs real-time face detection, \
gaze tracking, and person counting. Publishes analytics via embedded MQTT broker."
HOMEPAGE = "https://github.com/brightsign/bs-gaze-extension"
LICENSE = "PROPRIETARY"

inherit cmake pkgconfig

DEPENDS = " \
    opencv \
    gstreamer1.0 \
    gstreamer1.0-plugins-base \
    libmicrohttpd \
    mosquitto \
    boost \
    turbojpeg \
"

RDEPENDS:${PN} = " \
    opencv \
    gstreamer1.0 \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    mosquitto \
    ext-bundle \
"

S = "${WORKDIR}/git"
SRCREV = "${AUTOREV}"
BRANCH = "bus_watcher"

# Source from your git repo (adjust URL as needed)
SRC_URI = "git://github.com/brightsign/bs-gaze-extension.git;branch=${BRANCH};protocol=https"

# Or for local development, use:
# SRC_URI = "file://${TOPDIR}/../../brightsign-npu-gaze-extension"
# S = "${WORKDIR}"

# CMake configuration
EXTRA_OECMAKE = " \
    -DCMAKE_BUILD_TYPE=Release \
    -DTARGET_SOC=xt5 \
    -DLIB_ARCH=aarch64 \
"

# Installation
do_install:append() {
    install -d ${D}${bindir}
    install -m 0755 ${B}/attention_demo ${D}${bindir}/
    
    # Install configs
    install -d ${D}${datadir}/attention-demo/configs
    if [ -d "${S}/configs" ]; then
        install -m 0644 ${S}/configs/*.json ${D}${datadir}/attention-demo/configs/ || true
        install -m 0644 ${S}/configs/*.toml ${D}${datadir}/attention-demo/configs/ || true
        install -m 0644 ${S}/configs/*.yaml ${D}${datadir}/attention-demo/configs/ || true
    fi
    
    # Install models (if bundled)
    install -d ${D}${datadir}/attention-demo/model
    if [ -d "${S}/install/XT5/model" ]; then
        cp -r ${S}/install/XT5/model/* ${D}${datadir}/attention-demo/model/ || true
    fi
}

FILES:${PN} = " \
    ${bindir}/attention_demo \
    ${datadir}/attention-demo/* \
"

# Optional: create a systemd service if desired
# do_install:append() {
#     install -d ${D}${systemd_unitdir}/system
#     install -m 0644 ${S}/attention-demo.service ${D}${systemd_unitdir}/system/
# }
