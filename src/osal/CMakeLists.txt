add_library(obicall_osal STATIC
    osal_clock.c
    osal_process.c
    osal_ipc.c
    osal_dynload.c
    osal_file.c
    osal_thread.c
)

target_include_directories(obicall_osal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(obicall_osal PUBLIC Obicall::core)

if(WIN32)
    target_link_libraries(obicall_osal PUBLIC ws2_32)
else()
    target_link_libraries(obicall_osal PUBLIC ${CMAKE_DL_LIBS})
    find_package(Threads REQUIRED)
    target_link_libraries(obicall_osal PUBLIC Threads::Threads)
endif()
