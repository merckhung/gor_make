cmake_minimum_required(VERSION 3.10)
project(Calculator C CXX)

add_library(math STATIC src/math.c)
target_include_directories(math PUBLIC src)

add_executable(calculator src/main.c)
target_link_libraries(calculator PRIVATE math)
target_compile_options(calculator PRIVATE -Wall -O2)
