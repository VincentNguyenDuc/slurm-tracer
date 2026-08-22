find_package(Catch2 3 CONFIG REQUIRED)

add_executable(tests test.cpp)
target_include_directories(tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(tests PRIVATE Catch2::Catch2WithMain spdlog::spdlog)

include(CTest)
include(Catch)
catch_discover_tests(tests)
