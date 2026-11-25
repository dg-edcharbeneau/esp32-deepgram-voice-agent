# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# One board per IDF target, and one lock file per board. The two boards resolve
# different BSPs, and the component manager rewrites the lock whenever it
# re-solves, so a shared lock would flip between them on every set-target. The
# S3 keeps the original dependencies.lock so its history stays readable.
# IDF_TARGET is already known here: project.cmake resolves it on include.
if(NOT IDF_TARGET STREQUAL "esp32s3")
    idf_build_set_property(DEPENDENCIES_LOCK
        "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.${IDF_TARGET}")
endif()

project(deepgram_agent)
