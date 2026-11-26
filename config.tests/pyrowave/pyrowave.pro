QT -= core gui
CONFIG += console c++17 link_pkgconfig
CONFIG -= app_bundle
PKGCONFIG += pyrowave-shared
macx:!disable-prebuilts: INCLUDEPATH += $$PWD/../../libs/mac/include
SOURCES = main.cpp
