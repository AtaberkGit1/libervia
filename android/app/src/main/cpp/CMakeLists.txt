cmake_minimum_required(VERSION 3.22.1)
project("libervia_engine" C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(libervia_engine SHARED libervia_engine.c)

target_compile_options(libervia_engine PRIVATE
    -O3
    -Wall
    -Wextra
    -Wno-unused-parameter
    -ffunction-sections
    -fdata-sections
)

target_link_options(libervia_engine PRIVATE
    -Wl,-z,max-page-size=16384
)

find_library(log-lib log)

target_link_libraries(libervia_engine ${log-lib})
