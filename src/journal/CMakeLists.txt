add_executable(obicall-journald journal_main.c)
set_target_properties(obicall-journald PROPERTIES OUTPUT_NAME "obicall-journald")
target_include_directories(obicall-journald PRIVATE
    ${PROJECT_SOURCE_DIR}/src/osal
    ${PROJECT_SOURCE_DIR}/src/common
)
target_link_libraries(obicall-journald PRIVATE Obicall::core obicall_osal obicall_procutil)

install(TARGETS obicall-journald RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
