#ifndef PLUGIN_H
#define PLUGIN_H

#include "libtodmv.h"

/**
 * @brief searches for a plugin to handle the given line
 */
extern libtodmv_plugin_t* find_plugin( const char* line, const char* in_fname, int line_number, void* out_fp );


#endif // PLUGIN_H