add_executable(obicall_cli main.c)
set_target_properties(obicall_cli PROPERTIES OUTPUT_NAME "obicall")
target_include_directories(obicall_cli PRIVATE
    ${PROJECT_SOURCE_DIR}/src/osal
    ${PROJECT_SOURCE_DIR}/src/common
    ${PROJECT_SOURCE_DIR}/src/supervisor
)
target_link_libraries(obicall_cli PRIVATE Obicall::core obicall_osal obicall_procutil obicall_supervisor)
# `doctor` probes the installed provider directory relative to bin/ (see
# OBICALL_INSTALL_BIN_TO_LIBDIR in the top-level CMakeLists.txt).
target_compile_definitions(obicall_cli PRIVATE
    OBICALL_INSTALL_BIN_TO_LIBDIR="${OBICALL_INSTALL_BIN_TO_LIBDIR}")

install(TARGETS obicall_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
