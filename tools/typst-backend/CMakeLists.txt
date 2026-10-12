find_program(VTEXTEDIT_CARGO_EXECUTABLE cargo)
if(NOT VTEXTEDIT_CARGO_EXECUTABLE)
    message(FATAL_ERROR "Typst requires Cargo and the toolchain pinned in tools/typst-backend/rust-toolchain.toml")
endif()

set(VTEXTEDIT_TYPST_RUST_TARGET "" CACHE STRING "Rust target triple (required for cross compilation)")
if(CMAKE_CROSSCOMPILING AND NOT VTEXTEDIT_TYPST_RUST_TARGET)
    message(FATAL_ERROR "Set VTEXTEDIT_TYPST_RUST_TARGET when cross compiling the Typst helper")
endif()
set(_cargo_target_args)
set(_cargo_profile_dir "${CMAKE_BINARY_DIR}/typst-cargo/release")
if(VTEXTEDIT_TYPST_RUST_TARGET)
    list(APPEND _cargo_target_args --target "${VTEXTEDIT_TYPST_RUST_TARGET}")
    set(_cargo_profile_dir "${CMAKE_BINARY_DIR}/typst-cargo/${VTEXTEDIT_TYPST_RUST_TARGET}/release")
endif()
set(_cargo_helper "${_cargo_profile_dir}/vtextedit-typst${CMAKE_EXECUTABLE_SUFFIX}")
set(_runtime_helper "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/vtextedit-typst${CMAKE_EXECUTABLE_SUFFIX}")

add_custom_command(
    OUTPUT "${_runtime_helper}"
    BYPRODUCTS "${_cargo_helper}"
    COMMAND "${VTEXTEDIT_CARGO_EXECUTABLE}" build --release --locked
            --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
            --target-dir "${CMAKE_BINARY_DIR}/typst-cargo" ${_cargo_target_args}
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_cargo_helper}" "${_runtime_helper}"
    DEPENDS Cargo.toml Cargo.lock rust-toolchain.toml
            src/main.rs src/world.rs src/compile.rs src/preview.rs
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Building the pinned Typst compiler helper"
    VERBATIM
)
add_custom_target(vtextedit-typst ALL DEPENDS "${_runtime_helper}")
set(VTEXTEDIT_TYPST_HELPER "${_runtime_helper}" PARENT_SCOPE)
install(PROGRAMS "${_runtime_helper}" DESTINATION "${CMAKE_INSTALL_BINDIR}")
