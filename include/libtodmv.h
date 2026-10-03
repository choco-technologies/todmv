#ifndef LIBTODMV_H
#define LIBTODMV_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dmod_types.h"
#include "libtodmv_defs.h"
#include "libtodmv_types.h"
#include "libtodmv_plugin.h"

/**
 * Create a new libtodmv instance.
 *
 * @return A valid handle on success, or NULL on allocation failure.
 */
dmod_libtodmv_api(1.0, libtodmv_t, _create, ( void ));

/**
 * Destroy an instance created by libtodmv_create(). Safe to call with
 * NULL.
 */
dmod_libtodmv_api(1.0, void, _destroy, ( libtodmv_t context ));

/**
 * Example accessor - replace with your module's real API.
 *
 * @return true if handle is a valid, non-NULL instance.
 */
dmod_libtodmv_api(1.0, bool, _is_valid, ( libtodmv_t context ));

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
dmod_libtodmv_api(1.0, int, _assemble_line, (libtodmv_t context, const char* line, const char* in_fname, int line_number, void* out_fp ));

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
dmod_libtodmv_api(1.0, int, _assemble_file, (libtodmv_t context, const char* in_fname, const char* out_fname));

#endif // LIBTODMV_H
