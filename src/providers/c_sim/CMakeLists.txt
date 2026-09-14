add_library(provider_c_sim MODULE
    provider_c_sim.c
    ${PROJECT_SOURCE_DIR}/src/core/json_min.c
)

set_target_properties(provider_c_sim PROPERTIES
    PREFIX ""
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

target_include_directories(provider_c_sim PRIVATE ${PROJECT_SOURCE_DIR}/src/core)
target_link_libraries(provider_c_sim PRIVATE Obicall::core)
if(UNIX)
    target_link_libraries(provider_c_sim PRIVATE m)
endif()

install(TARGETS provider_c_sim LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/obicall/providers
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/obicall/providers)

# The examples/ pipeline config and manifest reference a stable filename
# under examples/providers/ so `obicall run --config examples/...` works
# from a fresh checkout without hardcoding a build directory path. This
# copies the freshly-built artifact there after every build; the copy is
# gitignored, not checked in.
add_custom_command(TARGET provider_c_sim POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PROJECT_SOURCE_DIR}/examples/providers
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:provider_c_sim> ${PROJECT_SOURCE_DIR}/examples/providers/
)
