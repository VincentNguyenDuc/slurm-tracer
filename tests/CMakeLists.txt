add_executable(tests
    main.cpp
    test_attribution.cpp
    test_record.cpp
)
target_link_libraries(tests PRIVATE st_core)

add_test(NAME unit COMMAND tests)
