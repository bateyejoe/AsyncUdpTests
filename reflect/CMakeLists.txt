cmake_minimum_required(VERSION 3.0.0)

project(reflect VERSION 0.1.0 LANGUAGES C CXX)

add_executable(reflect reflect.cpp)

set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
include(CPack)
