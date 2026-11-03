# Resolve a complete, pinned Bootlin collection before project() discovers any
# compiler state. Linux projects must not fall back to host compilers or tools.

include("${CMAKE_CURRENT_LIST_DIR}/pslog_lifecycle.cmake")

function(pslog_configure_bootlin_toolchain target_id)
    set(resolver "${CPKT_LIFECYCLE_SCRIPTS_DIR}/cpkt-toolchains.sh")
    set(selected_c_compiler "")
    set(selected_cxx_compiler "")
    if(NOT EXISTS "${resolver}")
        message(FATAL_ERROR "missing lifecycle toolchain resolver: ${resolver}")
    endif()

    execute_process(
        COMMAND "${resolver}" discover "${target_id}"
        RESULT_VARIABLE resolver_result
        OUTPUT_VARIABLE resolver_description
        ERROR_VARIABLE resolver_stderr
    )
    if(NOT resolver_result EQUAL 0)
        message(FATAL_ERROR
            "Unable to inspect the pinned Bootlin toolchain for ${target_id}.\n"
            "${resolver_stderr}")
    endif()
    if(NOT resolver_description MATCHES "(^|[\r\n])status=ready([\r\n]|$)")
        message(FATAL_ERROR
            "Pinned Bootlin collection ${target_id} is not prepared. Run: "
            "${resolver} ensure ${target_id}")
    endif()

    foreach(key cc cxx ld ar ranlib strip nm objcopy objdump addr2line readelf sysroot target_triple root)
        string(REGEX MATCH "(^|[\r\n])${key}=([^\r\n]+)" resolver_match "${resolver_description}")
        if(NOT resolver_match)
            message(FATAL_ERROR "Bootlin resolver did not report ${key} for ${target_id}")
        endif()
        set(bootlin_${key} "${CMAKE_MATCH_2}")
    endforeach()

    if(DEFINED PSLOG_BOOTLIN_C_COMPILER_OVERRIDE AND
       NOT PSLOG_BOOTLIN_C_COMPILER_OVERRIDE STREQUAL "")
        set(selected_c_compiler "${PSLOG_BOOTLIN_C_COMPILER_OVERRIDE}")
    else()
        set(selected_c_compiler "${bootlin_cc}")
    endif()
    if(DEFINED PSLOG_BOOTLIN_CXX_COMPILER_OVERRIDE AND
       NOT PSLOG_BOOTLIN_CXX_COMPILER_OVERRIDE STREQUAL "")
        set(selected_cxx_compiler "${PSLOG_BOOTLIN_CXX_COMPILER_OVERRIDE}")
    else()
        set(selected_cxx_compiler "${bootlin_cxx}")
    endif()
    if(NOT EXISTS "${selected_c_compiler}" OR
       NOT EXISTS "${selected_cxx_compiler}")
        message(FATAL_ERROR
            "The selected Bootlin compiler drivers are missing for ${target_id}: "
            "${selected_c_compiler}; ${selected_cxx_compiler}")
    endif()


    file(REAL_PATH "${bootlin_root}" bootlin_root_real)
    file(REAL_PATH "${bootlin_sysroot}" bootlin_sysroot_real)
    foreach(required_root IN ITEMS "${bootlin_root_real}" "${bootlin_sysroot_real}")
        if(NOT IS_DIRECTORY "${required_root}")
            message(FATAL_ERROR "Bootlin resolver returned a missing root for ${target_id}: ${required_root}")
        endif()
    endforeach()

    foreach(bootlin_driver IN ITEMS "${selected_c_compiler}" "${selected_cxx_compiler}")
        execute_process(
            COMMAND "${bootlin_driver}" -print-prog-name=ld
            RESULT_VARIABLE bootlin_linker_result
            OUTPUT_VARIABLE bootlin_reported_linker
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_VARIABLE bootlin_linker_error
        )
        if(NOT bootlin_linker_result EQUAL 0 OR NOT IS_ABSOLUTE "${bootlin_reported_linker}")
            message(FATAL_ERROR
                "Bootlin compiler did not report an absolute linker for ${target_id}: "
                "${bootlin_reported_linker} ${bootlin_linker_error}")
        endif()
        file(REAL_PATH "${bootlin_reported_linker}" bootlin_reported_linker_real)
        string(FIND "${bootlin_reported_linker_real}" "${bootlin_root_real}/" bootlin_linker_in_root)
        if(NOT bootlin_linker_in_root EQUAL 0)
            message(FATAL_ERROR
                "Bootlin compiler linker escaped its pinned root for ${target_id}: "
                "${bootlin_reported_linker_real} is not below ${bootlin_root_real}")
        endif()

        execute_process(
            COMMAND "${bootlin_driver}" "--sysroot=${bootlin_sysroot}" -print-file-name=libc.so
            RESULT_VARIABLE bootlin_libc_result
            OUTPUT_VARIABLE bootlin_reported_libc
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_VARIABLE bootlin_libc_error
        )
        if(NOT bootlin_libc_result EQUAL 0 OR NOT IS_ABSOLUTE "${bootlin_reported_libc}")
            message(FATAL_ERROR
                "Bootlin compiler did not report libc from its sysroot for ${target_id}: "
                "${bootlin_reported_libc} ${bootlin_libc_error}")
        endif()
        file(REAL_PATH "${bootlin_reported_libc}" bootlin_reported_libc_real)
        string(FIND "${bootlin_reported_libc_real}" "${bootlin_sysroot_real}/" bootlin_libc_in_sysroot)
        if(NOT bootlin_libc_in_sysroot EQUAL 0)
            message(FATAL_ERROR
                "Bootlin compiler libc escaped its configured sysroot for ${target_id}: "
                "${bootlin_reported_libc_real} is not below ${bootlin_sysroot_real}")
        endif()

    endforeach()

    set_property(GLOBAL PROPERTY PSLOG_VERIFIED_C_COMPILER "${selected_c_compiler}")
    set(CMAKE_C_COMPILER "${selected_c_compiler}" CACHE FILEPATH "" FORCE)
    set(CMAKE_CXX_COMPILER "${selected_cxx_compiler}" CACHE FILEPATH "" FORCE)
    set(CMAKE_C_COMPILER_TARGET "${bootlin_target_triple}" CACHE STRING "" FORCE)
    set(CMAKE_CXX_COMPILER_TARGET "${bootlin_target_triple}" CACHE STRING "" FORCE)
    set(CMAKE_LINKER "${bootlin_ld}" CACHE FILEPATH "" FORCE)
    set(CMAKE_AR "${bootlin_ar}" CACHE FILEPATH "" FORCE)
    set(CMAKE_RANLIB "${bootlin_ranlib}" CACHE FILEPATH "" FORCE)
    set(CMAKE_STRIP "${bootlin_strip}" CACHE FILEPATH "" FORCE)
    set(CMAKE_NM "${bootlin_nm}" CACHE FILEPATH "" FORCE)
    set(CMAKE_OBJCOPY "${bootlin_objcopy}" CACHE FILEPATH "" FORCE)
    set(CMAKE_OBJDUMP "${bootlin_objdump}" CACHE FILEPATH "" FORCE)
    set(CMAKE_ADDR2LINE "${bootlin_addr2line}" CACHE FILEPATH "" FORCE)
    set(CMAKE_READELF "${bootlin_readelf}" CACHE FILEPATH "" FORCE)
    set(CMAKE_SYSROOT "${bootlin_sysroot}" CACHE PATH "" FORCE)
    set(PSLOG_BOOTLIN_ROOT "${bootlin_root_real}" CACHE PATH
        "Pinned Bootlin collection root selected by the active toolchain" FORCE)
    set(PSLOG_BOOTLIN_SYSROOT "${bootlin_sysroot_real}" CACHE PATH
        "Pinned Bootlin sysroot selected by the active toolchain" FORCE)
    set(PSLOG_BOOTLIN_TOOLCHAIN TRUE CACHE BOOL
        "Whether this configuration uses a pinned Bootlin collection" FORCE)
    set(CMAKE_FIND_ROOT_PATH "${bootlin_sysroot}" "${bootlin_root}" CACHE STRING "" FORCE)
    set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER CACHE STRING "" FORCE)
    set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY CACHE STRING "" FORCE)
    set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY CACHE STRING "" FORCE)
    set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY CACHE STRING "" FORCE)
endfunction()
