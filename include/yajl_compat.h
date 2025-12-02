/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * yajl_compat.h: yajl-compatible API implemented using yyjson
 *
 */
#pragma once

#include <config.h>

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * yajl status codes
 */
typedef enum {
    yajl_status_ok = 0,
    yajl_status_client_canceled,
    yajl_status_error
} yajl_status;

/*
 * yajl parser config options
 */
typedef enum {
    yajl_allow_comments = 0x01,
    yajl_dont_validate_strings = 0x02,
    yajl_allow_trailing_garbage = 0x04,
    yajl_allow_multiple_values = 0x08,
    yajl_allow_partial_values = 0x10
} yajl_option;

/*
 * Generator status codes
 */
typedef enum {
    yajl_gen_status_ok = 0,
    yajl_gen_keys_must_be_strings,
    yajl_max_depth_exceeded,
    yajl_gen_in_error_state,
    yajl_gen_generation_complete,
    yajl_gen_invalid_number,
    yajl_gen_no_buf,
    yajl_gen_invalid_string
} yajl_gen_status;

/*
 * Opaque generator handle
 */
typedef struct yajl_gen_t *yajl_gen;

/*
 * Opaque parser handle
 */
typedef struct yajl_handle_t *yajl_handle;

/*
 * Callback function types for parsing
 */
typedef int (*yajl_null_func)(void *ctx);
typedef int (*yajl_boolean_func)(void *ctx, int boolVal);
typedef int (*yajl_integer_func)(void *ctx, long long integerVal);
typedef int (*yajl_double_func)(void *ctx, double doubleVal);
typedef int (*yajl_number_func)(void *ctx, const char *numberVal, size_t numberLen);
typedef int (*yajl_string_func)(void *ctx, const unsigned char *stringVal, size_t stringLen);
typedef int (*yajl_start_map_func)(void *ctx);
typedef int (*yajl_map_key_func)(void *ctx, const unsigned char *key, size_t stringLen);
typedef int (*yajl_end_map_func)(void *ctx);
typedef int (*yajl_start_array_func)(void *ctx);
typedef int (*yajl_end_array_func)(void *ctx);

/*
 * Callbacks structure for SAX-style parsing
 */
typedef struct {
    yajl_null_func yajl_null;
    yajl_boolean_func yajl_boolean;
    yajl_integer_func yajl_integer;
    yajl_double_func yajl_double;
    yajl_number_func yajl_number;
    yajl_string_func yajl_string;
    yajl_start_map_func yajl_start_map;
    yajl_map_key_func yajl_map_key;
    yajl_end_map_func yajl_end_map;
    yajl_start_array_func yajl_start_array;
    yajl_end_array_func yajl_end_array;
} yajl_callbacks;

/*
 * Generator functions
 */
yajl_gen yajl_gen_alloc(const void *allocFuncs);
void yajl_gen_free(yajl_gen g);
yajl_gen_status yajl_gen_get_buf(yajl_gen g, const unsigned char **buf, size_t *len);
void yajl_gen_clear(yajl_gen g);

yajl_gen_status yajl_gen_null(yajl_gen g);
yajl_gen_status yajl_gen_bool(yajl_gen g, int boolean);
yajl_gen_status yajl_gen_integer(yajl_gen g, long long number);
yajl_gen_status yajl_gen_double(yajl_gen g, double number);
yajl_gen_status yajl_gen_string(yajl_gen g, const unsigned char *str, size_t len);
yajl_gen_status yajl_gen_map_open(yajl_gen g);
yajl_gen_status yajl_gen_map_close(yajl_gen g);
yajl_gen_status yajl_gen_array_open(yajl_gen g);
yajl_gen_status yajl_gen_array_close(yajl_gen g);

/*
 * Parser functions
 */
yajl_handle yajl_alloc(const yajl_callbacks *callbacks,
                       const void *allocFuncs,
                       void *ctx);
void yajl_free(yajl_handle handle);
int yajl_config(yajl_handle handle, yajl_option opt, int value);
yajl_status yajl_parse(yajl_handle handle, const unsigned char *jsonText, size_t jsonTextLen);
yajl_status yajl_complete_parse(yajl_handle handle);
size_t yajl_get_bytes_consumed(yajl_handle handle);
unsigned char *yajl_get_error(yajl_handle handle, int verbose,
                              const unsigned char *jsonText, size_t jsonTextLen);
void yajl_free_error(yajl_handle handle, unsigned char *str);
