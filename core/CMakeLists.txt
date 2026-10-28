add_library(pluginrack_core STATIC src/Version.cpp)
add_library(pluginrack::core ALIAS pluginrack_core)
target_include_directories(pluginrack_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(pluginrack_core PRIVATE /W4 /WX)
