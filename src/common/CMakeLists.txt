add_library(obicall_procutil STATIC
    procutil.c
)

target_include_directories(obicall_procutil PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src/osal
)
target_link_libraries(obicall_procutil PUBLIC Obicall::core obicall_osal)

if(WIN32)
    target_link_libraries(obicall_procutil PUBLIC bcrypt)
endif()
