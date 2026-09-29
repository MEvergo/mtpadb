cmake_minimum_required(VERSION 3.20)
project(mtpadb LANGUAGES CXX)
include(GNUInstallDirs)

include(CTest)
add_subdirectory(linux)
add_subdirectory(protocol)
