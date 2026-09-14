add_executable(obicall-gated gate_main.c)
target_include_directories(obicall-gated PRIVATE
    ${PROJECT_SOURCE_DIR}/src/osal
    ${PROJECT_SOURCE_DIR}/src/common
)
target_link_libraries(obicall-gated PRIVATE Obicall::core obicall_osal obicall_procutil)

install(TARGETS obicall-gated RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
