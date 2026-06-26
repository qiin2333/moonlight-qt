QT += core network
CONFIG += c++17 console
CONFIG -= app_bundle

TARGET = moonlight-filemapping-smoke
TEMPLATE = app

include(../globaldefs.pri)

INCLUDEPATH += \
    $$PWD/../app \
    $$PWD/../moonlight-common-c/moonlight-common-c/src

win32 {
    contains(QT_ARCH, x86_64) {
        LIBS += -L$$PWD/../libs/windows/lib/x64
        INCLUDEPATH += $$PWD/../libs/windows/include/x64
    }
    contains(QT_ARCH, arm64) {
        LIBS += -L$$PWD/../libs/windows/lib/arm64
        INCLUDEPATH += $$PWD/../libs/windows/include/arm64
    }

    INCLUDEPATH += $$PWD/../libs/windows/include
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
    main.cpp \
    ../app/backend/identitymanager.cpp \
    ../app/backend/nvapp.cpp \
    ../app/backend/nvaddress.cpp \
    ../app/backend/nvcomputer.cpp \
    ../app/backend/nvhttp.cpp \
    ../app/backend/nvpairingmanager.cpp \
    ../app/settings/compatfetcher.cpp \
    ../app/streaming/filemappingclient.cpp

HEADERS += \
    ../app/backend/identitymanager.h \
    ../app/backend/nvapp.h \
    ../app/backend/nvaddress.h \
    ../app/backend/nvcomputer.h \
    ../app/backend/nvhttp.h \
    ../app/backend/nvpairingmanager.h \
    ../app/settings/compatfetcher.h \
    ../app/streaming/filemappingclient.h
