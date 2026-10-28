# The default is the declared vendored helper collection. Propagate an explicit
# selection into compiler probes; discovery never prepares or repairs tools.
if(NOT DEFINED CPKT_LIFECYCLE_SCRIPTS_DIR)
    get_filename_component(CPKT_LIFECYCLE_SCRIPTS_DIR
        "${CMAKE_CURRENT_LIST_DIR}/../../scripts" ABSOLUTE)
    set(CPKT_LIFECYCLE_SCRIPTS_DIR "${CPKT_LIFECYCLE_SCRIPTS_DIR}" CACHE PATH
        "Prepared lifecycle helper collection")
endif()
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CPKT_LIFECYCLE_SCRIPTS_DIR)
list(REMOVE_DUPLICATES CMAKE_TRY_COMPILE_PLATFORM_VARIABLES)
