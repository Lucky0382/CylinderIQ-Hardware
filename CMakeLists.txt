cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cylinderiq-hub-v2)
add_link_options("-Wl,--allow-multiple-definition")

