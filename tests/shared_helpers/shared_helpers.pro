QT += core network
QT -= gui
CONFIG += c++17 console
CONFIG -= app_bundle

TARGET = shared_helpers
TEMPLATE = app

INCLUDEPATH += $$PWD/../../app
SOURCES += main.cpp ../../app/settings/compatfetcher.cpp
HEADERS += ../../app/backend/pairedcertificate.h ../../app/versionutils.h ../../app/settings/compatfetcher.h
RESOURCES += ../clipboard_payload_routing/fixtures.qrc
