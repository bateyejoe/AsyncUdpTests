
cmake_minimum_required(VERSION 3.24)

# append CMAKE_MODULE_PATH so we could find IoUring package
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_package(IoUring 2.2 REQUIRED)

project(msim2 CXX)
add_executable(${PROJECT_NAME} msim2/msim2.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE IoUring::IoUring)
target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_17)

project(reflect CXX)
add_executable(${PROJECT_NAME} reflect/reflect.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE IoUring::IoUring)
target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_17)

