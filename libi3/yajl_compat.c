/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * yajl_compat.c: yajl-compatible API implemented using yyjson
 *
 */
#include "libi3.h"
#include "yajl_compat.h"

#include <yyjson.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Generator implementation using yyjson mutable document API.
 *
 * Bridges yajl's streaming API (separate key/value calls) with yyjson's tree
 * API by maintaining a stack of containers and buffering map keys.
 */
#define GEN_MAX_DEPTH 32

struct yajl_gen_t {
    yyjson_mut_doc *doc;
    yyjson_mut_val *stack[GEN_MAX_DEPTH]; /* container stack */
    int depth;
    bool in_map[GEN_MAX_DEPTH]; /* true if stack[i] is an object */
    char *pending_key;          /* buffered map key (owned) */
    size_t pending_key_len;
    char *buf; /* last serialization result (owned by yyjson) */
};

/* Add a value to the current container (or set as root) */
static void gen_add_value(const yajl_gen g, yyjson_mut_val *val) {
    if (g->depth == 0) {
        /* No container - this becomes the root */
        yyjson_mut_doc_set_root(g->doc, val);
        return;
    }

    yyjson_mut_val *container = g->stack[g->depth - 1];
    if (g->in_map[g->depth - 1]) {
        /* In an object - use pending key */
        yyjson_mut_val *key = yyjson_mut_strncpy(g->doc, g->pending_key, g->pending_key_len);
        yyjson_mut_obj_add(container, key, val);
        free(g->pending_key);
        g->pending_key = NULL;
        g->pending_key_len = 0;
    } else {
        /* In an array */
        yyjson_mut_arr_append(container, val);
    }
}

yajl_gen yajl_gen_alloc(const void *allocFuncs) {
    (void)allocFuncs;
    yajl_gen g = scalloc(1, sizeof(struct yajl_gen_t));
    g->doc = yyjson_mut_doc_new(NULL);
    return g;
}

void yajl_gen_free(yajl_gen g) {
    if (g) {
        yyjson_mut_doc_free(g->doc);
        free(g->pending_key);
        free(g->buf);
        free(g);
    }
}

yajl_gen_status yajl_gen_get_buf(yajl_gen g, const unsigned char **buf, size_t *len) {
    free(g->buf);
    g->buf = yyjson_mut_write(g->doc, 0, len);
    if (!g->buf) {
        /* Empty document - return empty string */
        g->buf = sstrdup("");
        *len = 0;
    }
    *buf = (const unsigned char *)g->buf;
    return yajl_gen_status_ok;
}

void yajl_gen_clear(yajl_gen g) {
    yyjson_mut_doc_free(g->doc);
    g->doc = yyjson_mut_doc_new(NULL);
    g->depth = 0;
    memset(g->in_map, 0, sizeof(g->in_map));
    free(g->pending_key);
    g->pending_key = NULL;
    g->pending_key_len = 0;
    free(g->buf);
    g->buf = NULL;
}

yajl_gen_status yajl_gen_null(yajl_gen g) {
    gen_add_value(g, yyjson_mut_null(g->doc));
    return yajl_gen_status_ok;
}

yajl_gen_status yajl_gen_bool(yajl_gen g, int boolean) {
    gen_add_value(g, yyjson_mut_bool(g->doc, boolean));
    return yajl_gen_status_ok;
}

yajl_gen_status yajl_gen_integer(yajl_gen g, long long number) {
    gen_add_value(g, yyjson_mut_sint(g->doc, number));
    return yajl_gen_status_ok;
}

yajl_gen_status yajl_gen_double(yajl_gen g, double number) {
    gen_add_value(g, yyjson_mut_real(g->doc, number));
    return yajl_gen_status_ok;
}

yajl_gen_status yajl_gen_string(yajl_gen g, const unsigned char *str, size_t len) {
    if (g->depth > 0 && g->in_map[g->depth - 1] && g->pending_key == NULL) {
        /* In an object and no pending key - this string is a key */
        g->pending_key = smalloc(len + 1);
        memcpy(g->pending_key, str, len);
        g->pending_key[len] = '\0';
        g->pending_key_len = len;
    } else {
        /* This string is a value */
        gen_add_value(g, yyjson_mut_strncpy(g->doc, (const char *)str, len));
    }
    return yajl_gen_status_ok;
}

yajl_gen_status yajl_gen_map_open(yajl_gen g) {
    yyjson_mut_val *obj = yyjson_mut_obj(g->doc);

    if (g->depth == 0) {
        yyjson_mut_doc_set_root(g->doc, obj);
    } else {
        gen_add_value(g, obj);
    }

    if (g->depth < GEN_MAX_DEPTH) {
        g->stack[g->depth] = obj;
        g->in_map[g->depth] = true;
        g->depth++;
    }
    return yajl_gen_status_ok;
}

yajl_gen_status yajl_gen_map_close(yajl_gen g) {
    if (g->depth > 0) {
        g->depth--;
    }
    return yajl_gen_status_ok;
}

yajl_gen_status yajl_gen_array_open(yajl_gen g) {
    yyjson_mut_val *arr = yyjson_mut_arr(g->doc);

    if (g->depth == 0) {
        yyjson_mut_doc_set_root(g->doc, arr);
    } else {
        gen_add_value(g, arr);
    }

    if (g->depth < GEN_MAX_DEPTH) {
        g->stack[g->depth] = arr;
        g->in_map[g->depth] = false;
        g->depth++;
    }
    return yajl_gen_status_ok;
}

yajl_gen_status yajl_gen_array_close(yajl_gen g) {
    if (g->depth > 0) {
        g->depth--;
    }
    return yajl_gen_status_ok;
}

/*
 * Parser implementation using yyjson DOM traversal
 */
struct yajl_handle_t {
    yajl_callbacks callbacks;
    void *ctx;
    char *error;
    unsigned int options; /* bitmask of yajl_option values */
    size_t bytes_consumed;
};

/* Forward declaration */
static yajl_status traverse_value(yajl_handle h, yyjson_val *val);

static yajl_status traverse_array(yajl_handle h, yyjson_val *arr) {
    if (h->callbacks.yajl_start_array) {
        if (!h->callbacks.yajl_start_array(h->ctx)) {
            return yajl_status_client_canceled;
        }
    }

    yyjson_val *val;
    yyjson_arr_iter iter;
    yyjson_arr_iter_init(arr, &iter);
    while ((val = yyjson_arr_iter_next(&iter))) {
        const yajl_status status = traverse_value(h, val);
        if (status != yajl_status_ok) {
            return status;
        }
    }

    if (h->callbacks.yajl_end_array) {
        if (!h->callbacks.yajl_end_array(h->ctx)) {
            return yajl_status_client_canceled;
        }
    }
    return yajl_status_ok;
}

static yajl_status traverse_object(yajl_handle h, yyjson_val *obj) {
    if (h->callbacks.yajl_start_map) {
        if (!h->callbacks.yajl_start_map(h->ctx)) {
            return yajl_status_client_canceled;
        }
    }

    yyjson_val *key;
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(obj, &iter);
    while ((key = yyjson_obj_iter_next(&iter))) {
        yyjson_val *val = yyjson_obj_iter_get_val(key);

        if (h->callbacks.yajl_map_key) {
            const char *k = yyjson_get_str(key);
            const size_t klen = yyjson_get_len(key);
            if (!h->callbacks.yajl_map_key(h->ctx, (const unsigned char *)k, klen)) {
                return yajl_status_client_canceled;
            }
        }

        const yajl_status status = traverse_value(h, val);
        if (status != yajl_status_ok) {
            return status;
        }
    }

    if (h->callbacks.yajl_end_map) {
        if (!h->callbacks.yajl_end_map(h->ctx)) {
            return yajl_status_client_canceled;
        }
    }
    return yajl_status_ok;
}

static yajl_status traverse_value(yajl_handle h, yyjson_val *val) {
    if (yyjson_is_null(val)) {
        if (h->callbacks.yajl_null) {
            if (!h->callbacks.yajl_null(h->ctx)) {
                return yajl_status_client_canceled;
            }
        }
    } else if (yyjson_is_bool(val)) {
        if (h->callbacks.yajl_boolean) {
            if (!h->callbacks.yajl_boolean(h->ctx, yyjson_get_bool(val))) {
                return yajl_status_client_canceled;
            }
        }
    } else if (yyjson_is_int(val)) {
        if (h->callbacks.yajl_integer) {
            if (!h->callbacks.yajl_integer(h->ctx, yyjson_get_sint(val))) {
                return yajl_status_client_canceled;
            }
        }
    } else if (yyjson_is_real(val)) {
        if (h->callbacks.yajl_double) {
            if (!h->callbacks.yajl_double(h->ctx, yyjson_get_real(val))) {
                return yajl_status_client_canceled;
            }
        }
    } else if (yyjson_is_str(val)) {
        if (h->callbacks.yajl_string) {
            const char *s = yyjson_get_str(val);
            const size_t len = yyjson_get_len(val);
            if (!h->callbacks.yajl_string(h->ctx, (const unsigned char *)s, len)) {
                return yajl_status_client_canceled;
            }
        }
    } else if (yyjson_is_arr(val)) {
        return traverse_array(h, val);
    } else if (yyjson_is_obj(val)) {
        return traverse_object(h, val);
    }
    return yajl_status_ok;
}

yajl_handle yajl_alloc(const yajl_callbacks *callbacks,
                       const void *allocFuncs,
                       void *ctx) {
    (void)allocFuncs;
    const yajl_handle h = smalloc(sizeof(struct yajl_handle_t));
    if (callbacks) {
        h->callbacks = *callbacks;
    } else {
        memset(&h->callbacks, 0, sizeof(h->callbacks));
    }
    h->ctx = ctx;
    h->error = NULL;
    h->options = 0;
    h->bytes_consumed = 0;
    return h;
}

void yajl_free(const yajl_handle handle) {
    if (handle) {
        free(handle->error);
        free(handle);
    }
}

int yajl_config(const yajl_handle handle, const yajl_option opt, const int value) {
    if (value) {
        handle->options |= opt;
    } else {
        handle->options &= ~opt;
    }
    return 1; /* success */
}

yajl_status yajl_parse(const yajl_handle handle, const unsigned char *jsonText, const size_t jsonTextLen) {
    yyjson_read_err err;

    /* Build yyjson flags based on yajl options */
    yyjson_read_flag flags = YYJSON_READ_ALLOW_TRAILING_COMMAS;
    if (handle->options & yajl_allow_comments) {
        flags |= YYJSON_READ_ALLOW_COMMENTS;
    }
    if (handle->options & yajl_allow_trailing_garbage) {
        flags |= YYJSON_READ_STOP_WHEN_DONE;
    }
    const bool allow_multiple = (handle->options & yajl_allow_multiple_values) != 0;
    if (allow_multiple) {
        flags |= YYJSON_READ_STOP_WHEN_DONE;
    }
    /* yajl_dont_validate_strings and yajl_allow_partial_values
     * don't have direct yyjson equivalents, ignore them */

    const char *ptr = (const char *)jsonText;
    size_t remaining = jsonTextLen;
    handle->bytes_consumed = 0;

    do {
        /* Skip leading whitespace between JSON values */
        while (remaining > 0 && (*ptr == ' ' || *ptr == '\t' ||
                                 *ptr == '\n' || *ptr == '\r')) {
            ptr++;
            remaining--;
            handle->bytes_consumed++;
        }
        if (remaining == 0) {
            break;
        }

        yyjson_doc *doc = yyjson_read_opts((char *)ptr, remaining, flags, NULL, &err);
        if (!doc) {
            free(handle->error);
            handle->error = (err.msg && err.msg[0]) ? sstrdup(err.msg) : sstrdup("parse error");
            handle->bytes_consumed += err.pos;
            return yajl_status_error;
        }

        yyjson_val *root = yyjson_doc_get_root(doc);
        yajl_status status = yajl_status_ok;
        if (root) {
            status = traverse_value(handle, root);
        }

        const size_t consumed = yyjson_doc_get_read_size(doc);
        handle->bytes_consumed += consumed;
        ptr += consumed;
        remaining -= consumed;

        yyjson_doc_free(doc);

        if (status != yajl_status_ok) {
            return status;
        }
    } while (allow_multiple && remaining > 0);

    return yajl_status_ok;
}

yajl_status yajl_complete_parse(const yajl_handle handle) {
    (void)handle;
    /* With DOM parsing, everything is done in yajl_parse */
    return yajl_status_ok;
}

size_t yajl_get_bytes_consumed(const yajl_handle handle) {
    return handle->bytes_consumed;
}

unsigned char *yajl_get_error(const yajl_handle handle, const int verbose, const unsigned char *jsonText, const size_t jsonTextLen) {
    (void)verbose;
    (void)jsonText;
    (void)jsonTextLen;
    if (handle->error) {
        return (unsigned char *)sstrdup(handle->error);
    }
    return (unsigned char *)sstrdup("unknown error");
}

void yajl_free_error(const yajl_handle handle, unsigned char *str) {
    (void)handle;
    free(str);
}
