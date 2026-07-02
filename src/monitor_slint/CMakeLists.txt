cmake_minimum_required(VERSION 3.21)
project(GameAgentMonitor VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Slint SDK path (set by user or auto-detect)
if(NOT DEFINED SLINT_DIR)
    set(SLINT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/slint_bin")
endif()

# Let find_package discover SlintConfig.cmake in lib/cmake/Slint
list(APPEND CMAKE_PREFIX_PATH "${SLINT_DIR}")
find_package(Slint REQUIRED)

# Source files
set(SOURCES main.cpp)

add_executable(monitor ${SOURCES})
target_link_libraries(monitor PRIVATE Slint::Slint)

# Compile .slint UI file
slint_target_sources(monitor appwindow.slint)

# Copy Slint DLL to output
add_custom_command(TARGET monitor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SLINT_DIR}/lib/slint_cpp.dll"
        "$<TARGET_FILE_DIR:monitor>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SLINT_DIR}/bin/slint_qt.dll"
        "$<TARGET_FILE_DIR:monitor>"
)
