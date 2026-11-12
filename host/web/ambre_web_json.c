/*
 * ambre_web_json.c — Minimal JSON writer (see ambre_web_json.h).
 */

#include "ambre_web_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void json_reserve(ambre_json_t *j, size_t extra)
{
    if (j->len + extra + 1u <= j->cap) return;
    size_t cap = j->cap ? j->cap : 4096u;
    while (j->len + extra + 1u > cap) cap *= 2u;
    char *p = (char *)realloc(j->buf, cap);
    if (!p) return;              /* keep what we have; the document is short */
    j->buf = p;
    j->cap = cap;
}

void ambre_json_init(ambre_json_t *j)
{
    memset(j, 0, sizeof(*j));
}

void ambre_json_free(ambre_json_t *j)
{
    free(j->buf);
    memset(j, 0, sizeof(*j));
}

void ambre_json_raw(ambre_json_t *j, const char *s)
{
    if (!s) return;
    size_t n = strlen(s);
    json_reserve(j, n);
    if (j->len + n + 1u > j->cap) return;
    memcpy(j->buf + j->len, s, n);
    j->len += n;
    j->buf[j->len] = '\0';
    /* Raw output is structure the caller manages ("[", ",\"runs\":["): it
     * resets the member separator, so the next key or fragment starts clean. */
    j->need_comma = false;
}

void ambre_json_ch(ambre_json_t *j, char c)
{
    json_reserve(j, 1u);
    if (j->len + 2u > j->cap) return;
    j->buf[j->len++] = c;
    j->buf[j->len] = '\0';
}

void ambre_json_str(ambre_json_t *j, const char *s)
{
    ambre_json_ch(j, '"');
    for (const char *p = s ? s : ""; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
        case '"':  ambre_json_raw(j, "\\\""); break;
        case '\\': ambre_json_raw(j, "\\\\"); break;
        case '\n': ambre_json_raw(j, "\\n"); break;
        case '\r': ambre_json_raw(j, "\\r"); break;
        case '\t': ambre_json_raw(j, "\\t"); break;
        default:
            if (c < 0x20u) {
                char esc[8];
                snprintf(esc, sizeof(esc), "\\u%04x", c);
                ambre_json_raw(j, esc);
            } else {
                ambre_json_ch(j, (char)c);
            }
        }
    }
    ambre_json_ch(j, '"');
}

static void json_key(ambre_json_t *j, const char *k)
{
    if (j->need_comma) ambre_json_ch(j, ',');
    ambre_json_str(j, k);
    ambre_json_ch(j, ':');
}

/* Every value writer ends by marking the member as written: the next key or
 * embedded fragment then knows it needs a separating comma. */
static void json_done(ambre_json_t *j)
{
    j->need_comma = true;
}

void ambre_json_kv_str(ambre_json_t *j, const char *k, const char *v)
{
    json_key(j, k);
    ambre_json_str(j, v);
    json_done(j);
}

void ambre_json_kv_u64(ambre_json_t *j, const char *k, uint64_t v)
{
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)v);
    json_key(j, k);
    ambre_json_raw(j, tmp);
    json_done(j);
}

void ambre_json_kv_i64(ambre_json_t *j, const char *k, int64_t v)
{
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
    json_key(j, k);
    ambre_json_raw(j, tmp);
    json_done(j);
}

void ambre_json_kv_dbl(ambre_json_t *j, const char *k, double v)
{
    char tmp[48];
    snprintf(tmp, sizeof(tmp), "%.6g", v);
    json_key(j, k);
    ambre_json_raw(j, tmp);
    json_done(j);
}

void ambre_json_kv_bool(ambre_json_t *j, const char *k, bool v)
{
    json_key(j, k);
    ambre_json_raw(j, v ? "true" : "false");
    json_done(j);
}

void ambre_json_kv_null(ambre_json_t *j, const char *k)
{
    json_key(j, k);
    ambre_json_raw(j, "null");
    json_done(j);
}

void ambre_json_embed(ambre_json_t *j, const ambre_json_t *sub)
{
    if (!sub || !sub->buf) return;
    if (j->need_comma) ambre_json_ch(j, ',');
    ambre_json_raw(j, sub->buf);
    json_done(j);
}

const char *ambre_json_publish(ambre_json_t *j)
{
    static char  *out = NULL;
    static size_t out_cap = 0u;

    size_t n = j->len;
    if (n + 1u > out_cap) {
        size_t cap = out_cap ? out_cap : 8192u;
        while (n + 1u > cap) cap *= 2u;
        char *p = (char *)realloc(out, cap);
        if (!p) return "{\"error\":\"out of memory\"}";
        out = p;
        out_cap = cap;
    }
    if (n > 0u) memcpy(out, j->buf, n);
    out[n] = '\0';
    return out;
}
