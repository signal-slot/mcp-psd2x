cmake_minimum_required(VERSION 3.16)
project(mcp-psd2x VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui McpServer PsdCore PsdGui PsdExporter)

qt_add_executable(mcp-psd2x
    main.cpp
)

target_link_libraries(mcp-psd2x PRIVATE
    Qt::Core
    Qt::Gui
    Qt::McpServer
    Qt::PsdCore
    Qt::PsdGui
    Qt::PsdExporter
)

# When building against local submodule builds, ensure their headers take
# priority over any system-installed (potentially older) QtPsd headers.
foreach(_pkg PsdCore PsdGui PsdExporter)
    if(TARGET Qt6::${_pkg})
        get_target_property(_inc Qt6::${_pkg} INTERFACE_INCLUDE_DIRECTORIES)
        if(_inc)
            target_include_directories(mcp-psd2x BEFORE PRIVATE ${_inc})
        endif()
    endif()
endforeach()
