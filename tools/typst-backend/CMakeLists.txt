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
            message(FATAL_ERROR "Unsupported Typst helper Apple architecture: ${_arch}")
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
    message(FATAL_ERROR "Set VTEXTEDIT_TYPST_RUST_TARGET when cross compiling the Typst helper")
endif()

set(_cargo_command "${VTEXTEDIT_CARGO_EXECUTABLE}")
if(APPLE)
    set(_cargo_command "${CMAKE_COMMAND}" -E env
        "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}"
        "${VTEXTEDIT_CARGO_EXECUTABLE}")
endif()
set(_cargo_commands)
set(_cargo_helpers)
if(_cargo_targets)
    foreach(_target IN LISTS _cargo_targets)
        list(APPEND _cargo_commands COMMAND ${_cargo_command} build --release --locked
            --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
            --target-dir "${CMAKE_BINARY_DIR}/typst-cargo" --target "${_target}")
        list(APPEND _cargo_helpers
            "${CMAKE_BINARY_DIR}/typst-cargo/${_target}/release/vtextedit-typst${CMAKE_EXECUTABLE_SUFFIX}")
    endforeach()
else()
    list(APPEND _cargo_commands COMMAND ${_cargo_command} build --release --locked
        --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
        --target-dir "${CMAKE_BINARY_DIR}/typst-cargo")
    list(APPEND _cargo_helpers
        "${CMAKE_BINARY_DIR}/typst-cargo/release/vtextedit-typst${CMAKE_EXECUTABLE_SUFFIX}")
endif()
set(_runtime_helper "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/vtextedit-typst${CMAKE_EXECUTABLE_SUFFIX}")
list(LENGTH _cargo_helpers _cargo_helper_count)
if(_cargo_helper_count GREATER 1)
    find_program(VTEXTEDIT_LIPO_EXECUTABLE lipo REQUIRED)
    set(_publish_command "${VTEXTEDIT_LIPO_EXECUTABLE}" -create ${_cargo_helpers}
        -output "${_runtime_helper}")
else()
    set(_publish_command "${CMAKE_COMMAND}" -E copy_if_different
        ${_cargo_helpers} "${_runtime_helper}")
endif()

add_custom_command(
    OUTPUT "${_runtime_helper}"
    BYPRODUCTS ${_cargo_helpers}
    ${_cargo_commands}
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    COMMAND ${_publish_command}
    DEPENDS Cargo.toml Cargo.lock rust-toolchain.toml
            src/main.rs src/world.rs src/compile.rs src/preview.rs src/highlight.rs
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Building the pinned Typst compiler helper"
    VERBATIM
)
add_custom_target(vtextedit-typst ALL DEPENDS "${_runtime_helper}")
set_target_properties(vtextedit-typst PROPERTIES VTEXTEDIT_TYPST_HELPER "${_runtime_helper}")
set(VTEXTEDIT_TYPST_HELPER "${_runtime_helper}" PARENT_SCOPE)
install(PROGRAMS "${_runtime_helper}" DESTINATION "${CMAKE_INSTALL_BINDIR}")
