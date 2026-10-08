# Production transport and backend dependencies shared by the probe and TLS regression.
include($$PWD/../../globaldefs.pri)

INCLUDEPATH += \
    $$PWD/../../app \
    $$PWD/../../moonlight-common-c/moonlight-common-c/src

win32 {
    contains(QT_ARCH, x86_64) {
        LIBS += -L$$PWD/../../libs/windows/lib/x64
        INCLUDEPATH += $$PWD/../../libs/windows/include/x64
    }
    contains(QT_ARCH, arm64) {
        LIBS += -L$$PWD/../../libs/windows/lib/arm64
        INCLUDEPATH += $$PWD/../../libs/windows/include/arm64
    }

    INCLUDEPATH += $$PWD/../../libs/windows/include
    LIBS += -llibssl -llibcrypto -lws2_32
}

unix:!macx {
    CONFIG += link_pkgconfig
    PKGCONFIG += openssl
}

macx {
    LIBS += -lssl.3 -lcrypto.3
}

SOURCES += \
    $$PWD/../../app/backend/identitymanager.cpp \
    $$PWD/../../app/backend/nvapp.cpp \
    $$PWD/../../app/backend/nvaddress.cpp \
    $$PWD/../../app/backend/nvcomputer.cpp \
    $$PWD/../../app/backend/nvhttp.cpp \
    $$PWD/../../app/backend/nvpairingmanager.cpp \
    $$PWD/../../app/settings/compatfetcher.cpp \
    $$PWD/../../app/streaming/filemappingclient.cpp \
    $$PWD/../../app/streaming/filemappingwebsocket.cpp

HEADERS += \
    $$PWD/../../app/backend/pairedcertificate.h \
    $$PWD/../../app/versionutils.h \
    $$PWD/../../app/backend/identitymanager.h \
    $$PWD/../../app/backend/nvapp.h \
    $$PWD/../../app/backend/nvaddress.h \
    $$PWD/../../app/backend/nvcomputer.h \
    $$PWD/../../app/backend/nvhttp.h \
    $$PWD/../../app/backend/nvpairingmanager.h \
    $$PWD/../../app/settings/compatfetcher.h \
    $$PWD/../../app/streaming/filemappingclient.h \
    $$PWD/../../app/streaming/filemappingwebsocket.h
