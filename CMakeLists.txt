cmake_minimum_required(VERSION 3.7)
project(netacka_clone)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(GUI_SOURCE
        private/gui/gui2/cmd.c
        private/gui/gui2/err.c
        private/gui/gui2/err.h
        private/gui/gui2/gui.h
        private/gui/gui2/gui2.c
        private/gui/gui2/net.c
        private/gui/gui2/read_line.c
        private/gui/gui2/read_line.h
        private/gui/gui3/cmd.c
        private/gui/gui3/err.c
        private/gui/gui3/err.h
        private/gui/gui3/gui.h
        private/gui/gui3/gui3.c
        private/gui/gui3/net.c
        private/gui/gui3/read_line.c
        private/gui/gui3/read_line.h
        private/gui/cmd.c
        private/gui/err.c
        private/gui/err.h
        private/gui/gui.h
        private/gui/gui1.c
        private/gui/net.c
        private/gui/read_line.c
        private/gui/read_line.h)

set(SOURCE_FILES
        rand.cc
        rand.h
        util.cc
        util.h
        protocol.cc
        protocol.h)

add_executable(netacka-server server.cc ${SOURCE_FILES})
add_executable(netacka-client client.cc ${SOURCE_FILES})