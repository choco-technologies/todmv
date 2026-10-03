#ifndef LIBTODMV_PLUGIN_H
#define LIBTODMV_PLUGIN_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dmod_types.h"
#include "libtodmv_defs.h"
#include "libtodmv_types.h"

/**
 * @brief Creates a new context for the plugin
 */
dmod_libtodmv_dif(1.0, libtodmv_plugin_ctx_t, _plugin_create, ());

/**
 * @brief destroys the plugin's context
 */
dmod_libtodmv_dif(1.0, void, _plugin_destroy, ( libtodmv_plugin_ctx_t context ));

/**
 * @brief interface for reading of commands supported by the given plugin
 * 
 * The function should return commands handled by the plugin
 * 
 * @param buffer            destination buffer for the commands
 * @param size              size of the buffer
 * 
 * @return number of handled opcodes
 */
dmod_libtodmv_dif(1.0, int, _plugin_supported_commands, ( libtodmv_cmd_t* buffer, size_t size ));

/**
 * @brief assemble line plugin 
 * 
 * Interface for pluging to assemble line
 * 
 * @param context       context of the plugin
 * @param opcode        opcode of the command
 * @param line          line to assemble
 * @param in_fname      input file name (for errors logging)
 * @param line_number   line number (for errors logging)
 * @param out_fp        output file pointer
 * 
 * @return 0 on success, -errno on error
 */
dmod_libtodmv_dif(1.0, int, _plugin_assemble_line, ( libtodmv_plugin_ctx_t context, libtodmv_opcode_t opcode, const char* line, const char* in_fname, int line_number, void* out_fp ));

#endif // LIBTODMV_PLUGIN_H