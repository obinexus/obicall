add_executable(obicall-workerd worker_main.c)
target_include_directories(obicall-workerd PRIVATE
    ${PROJECT_SOURCE_DIR}/src/osal
    ${PROJECT_SOURCE_DIR}/src/common
)
target_link_libraries(obicall-workerd PRIVATE Obicall::core obicall_osal obicall_procutil)

if(OBICALL_TEST_HOOKS)
    target_compile_definitions(obicall-workerd PRIVATE OBICALL_TEST_HOOKS_ENABLED=1)
endif()

install(TARGETS obicall-workerd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
