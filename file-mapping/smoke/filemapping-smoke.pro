QT += core network
CONFIG += c++17 console
CONFIG -= app_bundle

TARGET = moonlight-filemapping-smoke
TEMPLATE = app

include(filemapping-transport.pri)
SOURCES += main.cpp
