QT -= core gui
CONFIG += console c++17 link_pkgconfig
CONFIG -= app_bundle
TEMPLATE = app
TARGET = pyrowave_color_contract

PKGCONFIG += libavutil
INCLUDEPATH += ../../app ../../moonlight-common-c/moonlight-common-c/src
isEmpty(PYROWAVE_INCLUDEPATH) {
    !packagesExist(pyrowave-shared): error(AlkaidLab PyroWave headers are required)
    QMAKE_CXXFLAGS += $$system(pkg-config --cflags pyrowave-shared)
} else {
    INCLUDEPATH += $$PYROWAVE_INCLUDEPATH
}

SOURCES += main.cpp
HEADERS += ../../app/streaming/video/pyrowavecolor.h
