add_library(obicall_supervisor STATIC
    supervisor.c
    pipeline_config.c
    ${PROJECT_SOURCE_DIR}/src/core/json_min.c
)
target_include_directories(obicall_supervisor PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src/osal
    ${PROJECT_SOURCE_DIR}/src/common
    ${PROJECT_SOURCE_DIR}/src/core
)
target_link_libraries(obicall_supervisor PUBLIC Obicall::core obicall_osal obicall_procutil)
target_compile_definitions(obicall_supervisor PRIVATE OBICALL_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
