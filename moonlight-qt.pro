TEMPLATE = subdirs
SUBDIRS = \
    moonlight-common-c \
    qmdnsengine \
    app \
    h264bitstream

# The standalone bridge agent is opt-in while its platform backend is being
# migrated out of the in-process coordinator.
contains(CONFIG, remote_usb_agent) {
    usb_agent.file = usb-agent/usb_agent.pro
    SUBDIRS += usb_agent
}

# Build the dependencies in parallel before the final app
app.depends = qmdnsengine moonlight-common-c h264bitstream
contains(CONFIG, remote_usb_agent) {
    app.depends += usb_agent
}
!config_SL {
    SUBDIRS += clipboard-helper
    app.depends += clipboard-helper
}
win32:!winrt {
    SUBDIRS += AntiHooking
    app.depends += AntiHooking
}

# Support debug and release builds from command line for CI
CONFIG += debug_and_release

# Run our compile tests
load(configure)
qtCompileTest(SL)
qtCompileTest(EGL)
