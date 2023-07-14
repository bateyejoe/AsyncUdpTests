include (FindPkgConfig)
pkg_check_modules(LIBURING REQUIRED liburing)
cmake_minimum_required(VERSION 3.26)

project(msim2)

add_executable(msim2 msim2.cpp)

target_link_libraries(msim2 PUBLIC ${LIBURING_LIBRARIES})
target_include_directories(msim2 PRIVATE ${LIBURING_INCLUDE_DIRS})

