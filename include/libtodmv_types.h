#ifndef LIBTODMV_TYPES_H
#define LIBTODMV_TYPES_H

#include <stdint.h>

/**
 * Public API for the libtodmv module.
 *
 * Functions are declared with the dmod_libtodmv_api(...) macro - dmod's
 * standard pattern for functions callable from other modules (or from this
 * module's own tests/), resolved dynamically by the loader rather than
 * through normal static linkage. See dm_sw_ring/include/dm_sw_ring.h for a
 * fully worked real-world example of the same shape.
 *
 * Definitions in src/libtodmv.c use the matching
 * dmod_libtodmv_api_declaration(...) macro - a plain C function
 * definition here will NOT satisfy these declarations at link time.
 *
 * This is an example interface using the usual "opaque handle" pattern -
 * replace the handle, functions, and struct definition in
 * src/libtodmv.c with your module's real API.
 */

/* Opaque handle - the real struct is defined in src/libtodmv.c */
typedef struct libtodmv* libtodmv_t;

/**
 * @brief Assemble function
 * 
 * @param context       context of the library
 * @param line          line to assemble
 * @param fp            output file pointer
 * 
 * @return 0 on success, -errno on error
 */
typedef int (*libtodmv_assf_t)( libtodmv_t context, const char* line, void* fp );

/**
 * @brief type for storing opcodes
 */
typedef uint8_t libtodmv_opcode_t;

/**
 * @brief stores definition of the command
 */
typedef struct 
{
    const char*         command;        //!< Command to parse
    libtodmv_opcode_t   opcode;         //!< Opcode related with this command
    libtodmv_assf_t     assembly_f;     //!< Assembly function 
} libtodmv_instruction_t;

#endif // LIBTODMV_TYPES_H