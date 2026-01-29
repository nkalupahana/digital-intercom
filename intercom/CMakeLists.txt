# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.12)

file(STRINGS ".env" ENV_VARS)
foreach(ENV_VAR ${ENV_VARS})
    if(ENV_VAR AND ENV_VAR MATCHES "=")
        string(REPLACE "=" ";" ENV_VAR_PAIR ${ENV_VAR})
        list(LENGTH ENV_VAR_PAIR PAIR_LENGTH)
        if(PAIR_LENGTH EQUAL 2)
            list(GET ENV_VAR_PAIR 0 ENV_VAR_NAME)
            list(GET ENV_VAR_PAIR 1 ENV_VAR_VALUE)
            add_compile_definitions(${ENV_VAR_NAME}="${ENV_VAR_VALUE}")
        endif()
    endif()
endforeach()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(digital-intercom)