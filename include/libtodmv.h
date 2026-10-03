#ifndef LIBTODMV_H
#define LIBTODMV_H

#include "dmod_types.h"
#include "libtodmv_defs.h"
#include "libtodmv_types.h"

/**
 * libtodmv - converts dmview assembly (.dmvs) into the binary view format
 * (.dmv) and back.
 *
 * Nothing is ever held in memory as a whole: the source is read line by
 * line, the view is written as it is assembled, and validation and
 * disassembly read the view in small pieces. Only the view's tables (names,
 * variables, boxes, labels) are kept while assembling.
 */

/**
 * @brief Assemble a source into a binary view.
 *
 * Every error is reported to options->on_error with its file, line and
 * column; assembling stops after options->max_errors. When there are
 * errors, what was written to @p sink is incomplete and must be discarded.
 *
 * @return 0 on success, -EBADMSG when the source has errors, -EIO when the
 *         sink fails, -EINVAL for invalid arguments, -ENOMEM when out of
 *         memory.
 */
dmod_libtodmv_api(1.0, int, _assemble, ( const libtodmv_source_t* source, const libtodmv_sink_t* sink, const libtodmv_options_t* options, libtodmv_result_t* result ));

/**
 * @brief Assemble the file @p input into the file @p output.
 *
 * .include paths are relative to the directory of @p input unless
 * options->include_dir says otherwise. On failure @p output is removed.
 *
 * @return As libtodmv_assemble(), and -ENOENT when @p input cannot be opened.
 */
dmod_libtodmv_api(1.0, int, _assemble_file, ( const char* input, const char* output, const libtodmv_options_t* options, libtodmv_result_t* result ));

/**
 * @brief Check that @p input is a well-formed binary view.
 *
 * @param error_offset Receives the byte offset of the problem (may be NULL)
 * @param reason       Receives a short description of the problem (may be NULL)
 * @return 0 when valid, -EBADMSG when not, -EIO when reading fails,
 *         -ENOMEM when out of memory, -EINVAL for invalid arguments.
 */
dmod_libtodmv_api(1.0, int, _validate, ( const libtodmv_input_t* input, uint32_t* error_offset, const char** reason ));

/**
 * @brief libtodmv_validate() of a file. -ENOENT when it cannot be opened.
 */
dmod_libtodmv_api(1.0, int, _validate_file, ( const char* path, uint32_t* error_offset, const char** reason ));

/**
 * @brief Disassemble a binary view into equivalent assembly.
 *
 * Assembling the text again gives an equivalent view (its strings may be
 * stored in another order); the text is canonical - assembling it always
 * gives the same bytes. `.define` constants and comments are not part of
 * the binary and do not come back. Only sink->write is used.
 *
 * @return 0 on success, -EBADMSG when @p input is not a valid view, -EIO
 *         when reading or writing fails, -ENOMEM, -EINVAL.
 */
dmod_libtodmv_api(1.0, int, _disassemble, ( const libtodmv_input_t* input, const libtodmv_sink_t* sink ));

/**
 * @brief Disassemble the file @p input into the file @p output, or to the
 *        console when @p output is NULL.
 *
 * @return As libtodmv_disassemble(), and -ENOENT when @p input cannot be opened.
 */
dmod_libtodmv_api(1.0, int, _disassemble_file, ( const char* input, const char* output ));

#endif /* LIBTODMV_H */
