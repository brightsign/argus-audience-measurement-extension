SUMMARY = "BrightSign Mosquitto MQTT support"
DESCRIPTION = "Metapackage that provides mosquitto MQTT broker and client tools for IoT/telemetry integration"
LICENSE = "MIT"

inherit packagegroup

PACKAGES = "${PN}"

RDEPENDS:${PN} = " \
    mosquitto \
    mosquitto-broker \
    mosquitto-clients \
"

RSUGGESTS:${PN} = " \
    mosquitto-cpp \
"
