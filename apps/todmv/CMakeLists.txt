# ======================================================================
#               todmv - .dmvs <-> .dmv command-line tool
# ======================================================================
set(DMOD_MODULE_NAME        todmv)
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")
set(DMOD_STACK_SIZE         2048)
set(DMOD_PRIORITY           0)
set(DMOD_DMR_PATH           ${CMAKE_CURRENT_SOURCE_DIR}/todmv.dmr)
set(DMOD_MAL_IMPLS          )

dmod_add_executable(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    src/todmv.c
)

target_link_libraries(${DMOD_MODULE_NAME}
    libtodmv_if
)
