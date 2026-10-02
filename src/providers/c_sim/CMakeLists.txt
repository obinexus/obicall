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

# A second copy, installed next to the installed example manifest itself
# (${CMAKE_INSTALL_DATADIR}/obicall/examples/providers/provider_c_sim.*):
# provider_c_sim.manifest.json's "artifact_path" is resolved by
# obicall-workerd relative to the manifest's own directory (see
# dirname_of()/artifact_path in src/worker/worker_main.c), not relative to
# the lib/obicall/providers/ install above - that second location is only
# what `obicall doctor` probes. Without this, `obicall run`/`obicall demo`
# against the installed examples/position-fusion.json would fail to find
# the provider, exactly mirroring why the POST_BUILD copy below exists for
# the uninstalled checkout.
#
# Outside Windows that "copy" is a relative symlink to the lib/ install
# above: share/ must hold only architecture-independent files (FHS; Debian
# lintian rejects an ELF object there as arch-dependent-file-in-usr-share),
# and the worker's dlopen() follows the link. The link is relative so it
# stays valid under DESTDIR staging and in a relocated install.
if(WIN32)
    install(TARGETS provider_c_sim
        LIBRARY DESTINATION ${CMAKE_INSTALL_DATADIR}/obicall/examples/providers
        RUNTIME DESTINATION ${CMAKE_INSTALL_DATADIR}/obicall/examples/providers)
else()
    file(RELATIVE_PATH _provider_link_target
        "${CMAKE_INSTALL_FULL_DATADIR}/obicall/examples/providers"
        "${CMAKE_INSTALL_FULL_LIBDIR}/obicall/providers")
    set(_provider_link_dir "\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/obicall/examples/providers")
    install(CODE "
        file(MAKE_DIRECTORY \"${_provider_link_dir}\")
        message(STATUS \"Installing: ${_provider_link_dir}/$<TARGET_FILE_NAME:provider_c_sim> -> ${_provider_link_target}/$<TARGET_FILE_NAME:provider_c_sim>\")
        file(CREATE_LINK \"${_provider_link_target}/$<TARGET_FILE_NAME:provider_c_sim>\"
            \"${_provider_link_dir}/$<TARGET_FILE_NAME:provider_c_sim>\" SYMBOLIC)
    ")
endif()

# The examples/ pipeline config and manifest reference a stable filename
# under examples/providers/ so `obicall run --config examples/...` works
# from a fresh checkout without hardcoding a build directory path. This
# copies the freshly-built artifact there after every build; the copy is
# gitignored, not checked in.
add_custom_command(TARGET provider_c_sim POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PROJECT_SOURCE_DIR}/examples/providers
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:provider_c_sim> ${PROJECT_SOURCE_DIR}/examples/providers/
)
