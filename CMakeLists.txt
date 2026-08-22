cmake_minimum_required(VERSION 3.16)
project(MyProject C CXX)

set(CMAKE_C_FLAGS_RELEASE   "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

set(CMAKE_C_FLAGS_DEBUG   "-O0 -g")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g")

# RelWithDebInfo = profile build: optimized with frame pointers for flamegraphs
set(CMAKE_C_FLAGS_RELWITHDEBINFO   "-O2 -g -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -fno-omit-frame-pointer")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(OpenMP REQUIRED)

add_executable(main src/main.cpp)
target_include_directories(main PRIVATE include)
target_link_libraries(main PRIVATE fmt::fmt spdlog::spdlog OpenMP::OpenMP_CXX)

enable_testing()
add_subdirectory(tests)
