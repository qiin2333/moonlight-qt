QT -= core gui
CONFIG += c++17 console
CONFIG -= app_bundle
TEMPLATE = app
TARGET = video_texture_sampling
INCLUDEPATH += ../../app
SOURCES += main.cpp
HEADERS += ../../app/streaming/video/ffmpeg-renderers/videotexturesampling.h
