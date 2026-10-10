idf_component_register(
    SRCS "talk.cpp" "main.cpp" "tcpClient.cpp"
    INCLUDE_DIRS ""
)

set(ENV_FILE_PATH "${CMAKE_CURRENT_LIST_DIR}/../.env")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ENV_FILE_PATH})
file(STRINGS ${ENV_FILE_PATH} ENV_VARS)

foreach(ENV_VAR ${ENV_VARS})
    if(ENV_VAR AND ENV_VAR MATCHES "=")
        string(REPLACE "=" ";" ENV_VAR_PAIR ${ENV_VAR})
        list(LENGTH ENV_VAR_PAIR PAIR_LENGTH)

        if(PAIR_LENGTH EQUAL 2)
            list(GET ENV_VAR_PAIR 0 ENV_VAR_NAME)
            list(GET ENV_VAR_PAIR 1 ENV_VAR_VALUE)

            if(ENV_VAR_NAME STREQUAL "RADIO_ENABLED")
                string(TOLOWER "${ENV_VAR_VALUE}" RADIO_ENABLED_VALUE)
                if(RADIO_ENABLED_VALUE MATCHES "^(1|true|yes)$")
                    set(ENV_VAR_VALUE 1)
                elseif(RADIO_ENABLED_VALUE MATCHES "^(0|false|no)$")
                    set(ENV_VAR_VALUE 0)
                else()
                    message(FATAL_ERROR "RADIO_ENABLED must be true, false, 1, 0, yes, or no (got '${ENV_VAR_VALUE}')")
                endif()
            endif()

            target_compile_definitions(${COMPONENT_LIB} PRIVATE ${ENV_VAR_NAME}=${ENV_VAR_VALUE})
        endif()
    endif()
endforeach()
