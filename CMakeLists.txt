cmake_minimum_required(VERSION 3.20)
project(simpledb VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Automatically generate compile_commands.json for LSP (clangd)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Strict C compiler warning flags
function(enable_project_warnings TARGET_NAME)
    if(MSVC)
        target_compile_options(${TARGET_NAME} PRIVATE /W4 /WX)
    else()
        target_compile_options(${TARGET_NAME} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()

# Include directories
include_directories(include)

# Static Library Target
add_library(simpledb_lib STATIC src/db.c src/allocator.c src/mmap_file.c)
enable_project_warnings(simpledb_lib)

# Executable Target
add_executable(simpledb src/main.c)
target_link_libraries(simpledb PRIVATE simpledb_lib)
enable_project_warnings(simpledb)

# Testing
enable_testing()
add_subdirectory(tests)
