cmake_minimum_required(VERSION 3.28)
project(lsmkv LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

add_library(lsmkv_core STATIC src/store.cpp src/sstable.cpp)
target_include_directories(lsmkv_core PUBLIC src)

enable_testing()
add_executable(store_test tests/store_test.cpp)
target_link_libraries(store_test PRIVATE lsmkv_core)
add_test(NAME store_test COMMAND store_test)
