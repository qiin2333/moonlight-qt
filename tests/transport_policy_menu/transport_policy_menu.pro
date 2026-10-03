QT += core gui svg testlib
CONFIG += console c++17
CONFIG -= app_bundle
INCLUDEPATH += ../../app
SOURCES += main.cpp ../../app/streaming/video/overlaymenupanel.cpp ../../app/backend/transportpolicy.cpp
HEADERS += ../../app/streaming/video/overlayrasterwindow.h \
    ../../app/streaming/video/overlaymenupanel.h ../../app/backend/transportpolicy.h
RESOURCES += ../overlay_menu_navigation/fonts.qrc
DEFINES += TEST_DATA_DIR=\\\"$$PWD/../transport_policy/samples\\\"
