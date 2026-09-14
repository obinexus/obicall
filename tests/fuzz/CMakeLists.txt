set(OBICALL_FUZZ_TARGETS fuzz_wire_decode fuzz_manifest_parse)

foreach(t ${OBICALL_FUZZ_TARGETS})
    add_executable(${t} ${t}.c)
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${t} PRIVATE Obicall::core)

    if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
        target_compile_options(${t} PRIVATE -fsanitize=fuzzer,address,undefined)
        target_link_options(${t} PRIVATE -fsanitize=fuzzer,address,undefined)
        message(STATUS "${t}: building as a real libFuzzer target (Clang detected)")
        add_test(NAME ${t} COMMAND $<TARGET_FILE:${t}> -runs=20000)
    else()
        target_compile_definitions(${t} PRIVATE OBICALL_FUZZ_LITE_DRIVER=1)
        message(STATUS "${t}: Clang/libFuzzer not available with compiler "
                        "${CMAKE_C_COMPILER_ID} - building the standalone lite driver "
                        "(random input, not coverage-guided; see tests/fuzz/fuzz_driver.h)")
        add_test(NAME ${t} COMMAND $<TARGET_FILE:${t}> 1 20000)
    endif()
    set_tests_properties(${t} PROPERTIES TIMEOUT 60)
endforeach()
