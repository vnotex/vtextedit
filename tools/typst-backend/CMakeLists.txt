find_program(VTEXTEDIT_CARGO_EXECUTABLE cargo)
if(NOT VTEXTEDIT_CARGO_EXECUTABLE)
    message(FATAL_ERROR "Typst requires Cargo and the toolchain pinned in tools/typst-backend/rust-toolchain.toml")
endif()

set(VTEXTEDIT_TYPST_RUST_TARGET "" CACHE STRING "Rust target triple (required for cross compilation)")
set(_cargo_targets)
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    foreach(_arch IN LISTS CMAKE_OSX_ARCHITECTURES)
        if(_arch STREQUAL "x86_64")
            list(APPEND _cargo_targets x86_64-apple-darwin)
        elseif(_arch STREQUAL "arm64")
            list(APPEND _cargo_targets aarch64-apple-darwin)
        else()
            message(FATAL_ERROR "Unsupported Typst library Apple architecture: ${_arch}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _cargo_targets)
    if(VTEXTEDIT_TYPST_RUST_TARGET AND
       NOT "${VTEXTEDIT_TYPST_RUST_TARGET}" STREQUAL "${_cargo_targets}")
        message(FATAL_ERROR "VTEXTEDIT_TYPST_RUST_TARGET conflicts with CMAKE_OSX_ARCHITECTURES")
    endif()
elseif(VTEXTEDIT_TYPST_RUST_TARGET)
    list(APPEND _cargo_targets "${VTEXTEDIT_TYPST_RUST_TARGET}")
elseif(CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "Set VTEXTEDIT_TYPST_RUST_TARGET when cross compiling the Typst library")
endif()

set(_cargo_link_args)
if(WIN32)
    if(VTEXTEDIT_TYPST_RUST_TARGET AND NOT VTEXTEDIT_TYPST_RUST_TARGET MATCHES "-windows-msvc$")
        message(FATAL_ERROR "The Typst DLL requires an MSVC-compatible Rust target")
    endif()
    set(_cargo_filename vtextedit_typst.dll)
    set(_runtime_filename vtextedit_typst.dll)
elseif(APPLE)
    set(_cargo_filename libvtextedit_typst.dylib)
    set(_runtime_filename libvtextedit_typst.1.dylib)
    set(_link_filename libvtextedit_typst.dylib)
    set(_soname "@rpath/${_runtime_filename}")
    list(APPEND _cargo_link_args -C "link-arg=-Wl,-install_name,${_soname}"
        -C "link-arg=-Wl,-compatibility_version,1" -C "link-arg=-Wl,-current_version,1.0.0")
else()
    set(_cargo_filename libvtextedit_typst.so)
    set(_runtime_filename libvtextedit_typst.so.1)
    set(_link_filename libvtextedit_typst.so)
    set(_soname "${_runtime_filename}")
    list(APPEND _cargo_link_args -C "link-arg=-Wl,-soname,${_soname}")
endif()

set(_cargo_command "${VTEXTEDIT_CARGO_EXECUTABLE}")
if(APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET)
    set(_cargo_command "${CMAKE_COMMAND}" -E env
        "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}"
        "${VTEXTEDIT_CARGO_EXECUTABLE}")
endif()
set(_cargo_commands)
set(_cargo_libraries)
set(_cargo_byproducts)
if(NOT _cargo_targets)
    # The host target uses Cargo's non-triple release directory.
    set(_cargo_targets host)
endif()
foreach(_target IN LISTS _cargo_targets)
    set(_target_args)
    set(_target_directory "${CMAKE_BINARY_DIR}/typst-cargo")
    if(NOT _target STREQUAL "host")
        list(APPEND _target_args --target "${_target}")
        string(APPEND _target_directory "/${_target}")
    endif()
    # rustc's trailing arguments apply only to this cdylib, not its dependency
    # graph, and preserve the caller's toolchain/linker environment.
    list(APPEND _cargo_commands COMMAND ${_cargo_command} rustc --lib --release --locked
        --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
        --target-dir "${CMAKE_BINARY_DIR}/typst-cargo" ${_target_args}
        -- ${_cargo_link_args})
    list(APPEND _cargo_libraries "${_target_directory}/release/${_cargo_filename}")
    if(WIN32)
        set(_cargo_implib "${_target_directory}/release/vtextedit_typst.dll.lib")
        list(APPEND _cargo_byproducts "${_cargo_implib}")
    endif()
endforeach()
list(APPEND _cargo_byproducts ${_cargo_libraries})

set(_runtime_directory "${CMAKE_CURRENT_BINARY_DIR}/runtime")
set(_runtime_library "${_runtime_directory}/${_runtime_filename}")
set(_runtime_byproducts "${_runtime_library}")
set(_publish_commands COMMAND "${CMAKE_COMMAND}" -E make_directory "${_runtime_directory}")
list(LENGTH _cargo_libraries _cargo_library_count)
if(_cargo_library_count GREATER 1)
    find_program(VTEXTEDIT_LIPO_EXECUTABLE lipo REQUIRED)
    list(APPEND _publish_commands COMMAND "${VTEXTEDIT_LIPO_EXECUTABLE}" -create ${_cargo_libraries}
        -output "${_runtime_library}")
else()
    list(APPEND _publish_commands COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        ${_cargo_libraries} "${_runtime_library}")
endif()
if(WIN32)
    set(_runtime_implib "${_runtime_directory}/vtextedit_typst.dll.lib")
    list(APPEND _runtime_byproducts "${_runtime_implib}")
    list(APPEND _publish_commands COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${_cargo_implib}" "${_runtime_implib}")
else()
    set(_runtime_link "${_runtime_directory}/${_link_filename}")
    list(APPEND _runtime_byproducts "${_runtime_link}")
    list(APPEND _publish_commands COMMAND "${CMAKE_COMMAND}" -E create_symlink
        "${_runtime_filename}" "${_runtime_link}")
endif()

# Always ask Cargo to check its complete dependency graph. This also recreates
# removed Cargo/staging artifacts and refreshes consumers without a C++ relink.
add_custom_target(vtextedit_typst_build ALL
    ${_cargo_commands}
    ${_publish_commands}
    BYPRODUCTS ${_cargo_byproducts} ${_runtime_byproducts}
    DEPENDS Cargo.toml Cargo.lock rust-toolchain.toml
            src/lib.rs src/ffi.rs src/world.rs src/compile.rs src/fragment.rs src/preview.rs src/highlight.rs src/resources.rs
            include/vtextedit_typst.h
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Building the pinned Typst shared library"
    VERBATIM
)
add_library(VTextEditTypst SHARED IMPORTED GLOBAL)
set_target_properties(VTextEditTypst PROPERTIES
    IMPORTED_LOCATION "${_runtime_library}"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/include")
if(WIN32)
    set_target_properties(VTextEditTypst PROPERTIES IMPORTED_IMPLIB "${_runtime_implib}")
    # VNote uses a flat runtime install directory; never put its DLL under lib/.
    install(FILES "${_runtime_library}" DESTINATION "${CMAKE_INSTALL_BINDIR}")
    install(FILES "${_runtime_implib}" DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Development)
else()
    set_target_properties(VTextEditTypst PROPERTIES IMPORTED_SONAME "${_soname}")
    install(FILES "${_runtime_library}" "${_runtime_link}" DESTINATION "${CMAKE_INSTALL_LIBDIR}")
endif()
add_dependencies(VTextEditTypst vtextedit_typst_build)
install(FILES include/vtextedit_typst.h DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/vtextedit" COMPONENT Development)
