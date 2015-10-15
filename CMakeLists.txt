cmake_minimum_required(VERSION 2.8.8)

project(Gormake)
include(FindThreads)

set(CMAKE_BUILD_TYPE "Debug")

# Revision
set(GORMAKE_VERSION_MAJOR 1)
set(GORMAKE_VERSION_MINOR 0)
set(GORMAKE_VERSION_PATCH 0)

# Directories
include_directories(${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/libgormake)

# CXX FLAGS
set(CMAKE_CXX_FLAGS "-std=c++11 -Wall -g3")

add_subdirectory(libgormake)
add_subdirectory(gor_make)

