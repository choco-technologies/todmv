#define DMOD_ENABLE_REGISTRATION ON
#include "dmod.h"
#include "libtodmv.h"
#include <errno.h>

#define MAGIC_NUMBER        0x43129999

/* Example internal state - replace with your module's real fields. */
struct libtodmv
{
    uint32_t        magic;  //!< Magic number for context verification
};

/**
 * @brief creates new context of the library (for a new app)
 */
dmod_libtodmv_api_declaration(1.0, libtodmv_t, _create, ( void ))
{
    /* Dmod_Malloc/Dmod_Free (SAL) are dmod's own heap functions - embedded
     * targets don't necessarily link a libc allocator, so use these instead
     * of malloc()/free() in module code. */
    struct libtodmv *instance = Dmod_Malloc(sizeof(*instance));
    if (instance == NULL)
    {
        return NULL;
    }

    instance->magic = MAGIC_NUMBER;
    return instance;
}

/**
 * @brief destroys the context of the library
 */
dmod_libtodmv_api_declaration(1.0, void, _destroy, ( libtodmv_t handle ))
{
    if(libtodmv_is_valid(handle))
    {
        handle->magic = 0;
        Dmod_Free(handle);
    }
}

/**
 * @brief checks if the given context is valid
 */
dmod_libtodmv_api_declaration(1.0, bool, _is_valid, ( libtodmv_t handle ))
{
    return handle != NULL && handle->magic == MAGIC_NUMBER;
}

/**
 * @brief assebmles the next line of the script
 * 
 * The function is responsible for parsing of the one line of the `*.dmvs` binary. 
 * 
 * @param context           Context of the library
 * @param line              Line to assemble
 * @param in_fname          Input file name (for error messages)
 * @param line_number       Line number (required for error messages)
 * @param out_fp            Output file pointer 
 * 
 * @return 0 on success, -errno on error 
 */
dmod_libtodmv_api_declaration(1.0, int, _assemble_line, (libtodmv_t context, const char* line, const char* in_fname, int line_number, void* out_fp ))
{
    if(!libtodmv_is_valid(context))
    {
        DMOD_LOG_ERROR("Invalid context\n");
        return -EINVAL;
    }

    

    return 0;
}

/**
 * @brief assembles the file 
 * 
 * The function converts the given input file into the `*.dmv` binary.
 * 
 * @param context           Context of the library
 * @param in_fname          Input file name
 * @param out_fname         Output file name
 * 
 * @return 0 on success, -errno on error
 */
dmod_libtodmv_api_declaration(1.0, int, _assemble_file, (libtodmv_t context, const char* in_fname, const char* out_fname))
{

}


/**
 * @brief Initialization function for the module.
 *
 * This function is called when the module is enabled.
 * Please use this function to initialize the module, for instance:
 * - initialize the module variables
 * - initialize the module hardware
 * - allocate memory
 */
int dmod_init(const Dmod_Config_t *Config)
{
    return 0;
}

/**
 * @brief De-initialization function for the module.
 *
 * This function is called when the module is disabled.
 * Please use this function to de-initialize the module, for instance:
 * - free memory
 * - de-initialize the module hardware
 * - de-initialize the module variables
 */
int dmod_deinit(void)
{
    return 0;
}
