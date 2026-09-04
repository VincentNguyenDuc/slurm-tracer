add_executable(tests
    main.cpp
)
target_link_libraries(tests PRIVATE st_core)

add_test(NAME unit COMMAND tests)
