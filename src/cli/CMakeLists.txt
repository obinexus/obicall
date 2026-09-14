add_executable(obicall_cli main.c)
set_target_properties(obicall_cli PROPERTIES OUTPUT_NAME "obicall")
target_include_directories(obicall_cli PRIVATE
    ${PROJECT_SOURCE_DIR}/src/osal
    ${PROJECT_SOURCE_DIR}/src/common
    ${PROJECT_SOURCE_DIR}/src/supervisor
)
target_link_libraries(obicall_cli PRIVATE Obicall::core obicall_osal obicall_procutil obicall_supervisor)

install(TARGETS obicall_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
