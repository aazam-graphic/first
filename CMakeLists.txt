cmake_minimum_required(VERSION 3.22)
set(IDF_TARGET "esp32s3")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Trim the build to only the components we actually need
idf_build_set_property(MINIMAL_BUILD ON)
project(xbox360_controller)
# SPIFFS image - Solution 2A: wavs moved from EMBED_FILES to SPIFFS partition (storage 9.8M)
spiffs_create_partition_image(storage spiffs FLASH_IN_PROJECT)