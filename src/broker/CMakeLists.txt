add_executable(obicall-brokerd broker_main.c)
target_include_directories(obicall-brokerd PRIVATE
    ${PROJECT_SOURCE_DIR}/src/osal
    ${PROJECT_SOURCE_DIR}/src/common
)
target_link_libraries(obicall-brokerd PRIVATE Obicall::core obicall_osal obicall_procutil)
if(UNIX)
    target_link_libraries(obicall-brokerd PRIVATE m)
endif()

install(TARGETS obicall-brokerd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
