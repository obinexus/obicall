add_library(obicall_core SHARED
    status.c
    digest.c
    observation.c
    wire.c
    json_min.c
    manifest.c
    estimator.c
    dgt.c
    journal.c
    gate.c
    runtime.c
)
add_library(Obicall::core ALIAS obicall_core)

set_target_properties(obicall_core PROPERTIES
    OUTPUT_NAME obicall
    PREFIX ""
    IMPORT_PREFIX ""
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    WINDOWS_EXPORT_ALL_SYMBOLS OFF
)

target_include_directories(obicall_core
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(obicall_core PRIVATE
    OBICALL_CORE_BUILD=1
    OBICALL_VERSION_STRING="${PROJECT_VERSION}"
)

generate_export_header(obicall_core
    BASE_NAME OBICALL
    EXPORT_MACRO_NAME OBICALL_API
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/obicall/obicall_export.h"
)

install(TARGETS obicall_core
    EXPORT ObicallTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/obicall DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/obicall/obicall_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/obicall)
