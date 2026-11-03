TEMPLATE = app
TARGET = ax7zfb-direct-host-tests
CONFIG += console c++11
CONFIG -= app_bundle
QT += core gui
SOURCES = direct-host-tests.cpp
HEADERS = ../ax7zsinglesource.h ../ax7zflipbuffer.h
INCLUDEPATH += ..
