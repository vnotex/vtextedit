add_subdirectory(syntax-highlighting)
add_subdirectory(katevi)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(cmark)

add_subdirectory(sonnet)
