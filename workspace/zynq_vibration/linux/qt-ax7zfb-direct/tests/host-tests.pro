TEMPLATE = app
TARGET = ax7zfb-host-tests
CONFIG += console c++11
CONFIG -= app_bundle
QT += core gui
SOURCES = host-tests.cpp
HEADERS = ../ax7zflipbuffer.h
INCLUDEPATH += ..
