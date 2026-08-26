# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(deepgram_agent)

#
# Espressif's own AEC test vectors, in the `storage` partition.
#
# aec_in_far.wav / aec_in_near.wav are a far/near pair; aec_test_sr.wav and
# aec_test_fd.wav are the outputs Espressif's own AEC produces from them. Running
# our integration against the same pair and comparing ERLE with theirs is what
# separates "our wiring is wrong" from "this room cannot be cancelled" -- two
# questions every measurement in this project had conflated until now.
#
# NOT flashed with the app. FLASH_IN_PROJECT would move 5 MB on every `idf.py
# flash` for data that changes never; use `idf.py storage-flash` once instead.
# The partition already existed and was unused, so partitions.csv is untouched and
# the byte-identical layout shared with spec_analyzer_radial still holds.
#
if(CONFIG_AEC_BENCH)
    spiffs_create_partition_image(storage ${CMAKE_CURRENT_SOURCE_DIR}/aec_vectors)
endif()
