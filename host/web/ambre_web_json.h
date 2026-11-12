/*
 * ambre_web_json.h — Minimal JSON writer for the browser API.
 *
 * The web page reads JSON documents, never binary structures: the C side owns
 * the numbers, the browser only draws them. No dependency, no allocation
 * besides the buffer itself.
 */

#ifndef AMBRE_WEB_JSON_H
#define AMBRE_WEB_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
    bool   need_comma;   /* a member was written: the next one needs a comma */
} ambre_json_t;

void        ambre_json_init(ambre_json_t *j);
void        ambre_json_free(ambre_json_t *j);
void        ambre_json_raw(ambre_json_t *j, const char *s);
void        ambre_json_ch(ambre_json_t *j, char c);
void        ambre_json_str(ambre_json_t *j, const char *s);
void        ambre_json_embed(ambre_json_t *j, const ambre_json_t *sub);
void        ambre_json_kv_str(ambre_json_t *j, const char *k, const char *v);
void        ambre_json_kv_u64(ambre_json_t *j, const char *k, uint64_t v);
void        ambre_json_kv_i64(ambre_json_t *j, const char *k, int64_t v);
void        ambre_json_kv_dbl(ambre_json_t *j, const char *k, double v);
void        ambre_json_kv_bool(ambre_json_t *j, const char *k, bool v);
void        ambre_json_kv_null(ambre_json_t *j, const char *k);

/* Copy the document into a static buffer and return it. The pointer stays
 * valid until the next publish(): the browser copies the bytes out at once. */
const char *ambre_json_publish(ambre_json_t *j);

#endif /* AMBRE_WEB_JSON_H */
