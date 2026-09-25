cmake_minimum_required(VERSION 3.5)

# sdkconfig.local (gitignored) carries real WiFi credentials, kept out of
# sdkconfig.defaults specifically because that file IS committed. Must be
# set before the project.cmake include below, which is what actually reads
# SDKCONFIG_DEFAULTS.
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.local")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

add_compile_options(-fdiagnostics-color=always)

project(box3_dashboard)
