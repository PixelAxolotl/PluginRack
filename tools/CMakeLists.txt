add_executable(pluginrack-render render_cli/main.cpp)
target_link_libraries(pluginrack-render PRIVATE pluginrack::core)
