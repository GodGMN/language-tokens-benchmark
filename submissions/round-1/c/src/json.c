#include "json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* allocation                                                          */
/* ------------------------------------------------------------------ */

void jv_oom(void)
{
    static const char msg[] =
        "{\"status\":\"failed\",\"results\":{},\"execution_order\":[],"
        "\"skipped\":[],\"error\":{\"code\":\"INTERNAL_ERROR\",\"step\":null,"
        "\"message\":\"out of memory\"}}\n";
    fwrite(msg, 1, sizeof(msg) - 1, stdout);
    fflush(stdout);
    exit(3);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        jv_oom();
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q)
        jv_oom();
    return q;
}

static char *xstrndup(const char *s, size_t n)
{
    char *p = xmalloc(n + 1);
    if (n)
        memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/* ------------------------------------------------------------------ */
/* constructors                                                        */
/* ------------------------------------------------------------------ */

static JVal *jv_new(JType t)
{
    JVal *v = xmalloc(sizeof(*v));
    memset(v, 0, sizeof(*v));
    v->type = t;
    return v;
}

JVal *jv_null(void) { return jv_new(J_NULL); }

JVal *jv_bool(int b)
{
    JVal *v = jv_new(J_BOOL);
    v->u.b = b ? 1 : 0;
    return v;
}

JVal *jv_int(int64_t i)
{
    JVal *v = jv_new(J_NUM);
    v->u.n.is_int = 1;
    v->u.n.i = i;
    v->u.n.d = (double)i;
    return v;
}

JVal *jv_double(double d)
{
    JVal *v = jv_new(J_NUM);
    v->u.n.is_int = 0;
    v->u.n.d = d;
    v->u.n.i = 0;
    return v;
}

JVal *jv_str(const char *s, size_t len)
{
    JVal *v = jv_new(J_STR);
    v->u.s.s = xstrndup(s ? s : "", len);
    v->u.s.len = len;
    return v;
}

JVal *jv_cstr(const char *s) { return jv_str(s, strlen(s)); }

JVal *jv_arr(void)
{
    JVal *v = jv_new(J_ARR);
    v->u.a.cap = 4;
    v->u.a.v = xmalloc(v->u.a.cap * sizeof(JVal *));
    v->u.a.n = 0;
    return v;
}

JVal *jv_obj(void)
{
    JVal *v = jv_new(J_OBJ);
    v->u.o.cap = 4;
    v->u.o.k = xmalloc(v->u.o.cap * sizeof(JStr));
    v->u.o.v = xmalloc(v->u.o.cap * sizeof(JVal *));
    v->u.o.n = 0;
    return v;
}

void jv_arr_push(JVal *arr, JVal *v)
{
    if (arr->type != J_ARR)
        return;
    if (arr->u.a.n == arr->u.a.cap) {
        arr->u.a.cap *= 2;
        arr->u.a.v = xrealloc(arr->u.a.v, arr->u.a.cap * sizeof(JVal *));
    }
    arr->u.a.v[arr->u.a.n++] = v;
}

static void obj_grow(JVal *o)
{
    if (o->u.o.n == o->u.o.cap) {
        o->u.o.cap *= 2;
        o->u.o.k = xrealloc(o->u.o.k, o->u.o.cap * sizeof(JStr));
        o->u.o.v = xrealloc(o->u.o.v, o->u.o.cap * sizeof(JVal *));
    }
}

void jv_obj_append(JVal *obj, const char *key, size_t klen, JVal *v)
{
    if (obj->type != J_OBJ)
        return;
    obj_grow(obj);
    obj->u.o.k[obj->u.o.n].s = xstrndup(key, klen);
    obj->u.o.k[obj->u.o.n].len = klen;
    obj->u.o.v[obj->u.o.n] = v;
    obj->u.o.n++;
}

void jv_obj_set(JVal *obj, const char *key, size_t klen, JVal *v)
{
    if (obj->type != J_OBJ)
        return;
    for (size_t i = 0; i < obj->u.o.n; i++) {
        if (obj->u.o.k[i].len == klen &&
            (klen == 0 || memcmp(obj->u.o.k[i].s, key, klen) == 0)) {
            obj->u.o.v[i] = v;
            return;
        }
    }
    jv_obj_append(obj, key, klen, v);
}

JVal *jv_obj_get_len(const JVal *obj, const char *key, size_t klen)
{
    if (!obj || obj->type != J_OBJ)
        return NULL;
    for (size_t i = 0; i < obj->u.o.n; i++) {
        if (obj->u.o.k[i].len == klen &&
            (klen == 0 || memcmp(obj->u.o.k[i].s, key, klen) == 0))
            return obj->u.o.v[i];
    }
    return NULL;
}

JVal *jv_obj_get(const JVal *obj, const char *key)
{
    return jv_obj_get_len(obj, key, strlen(key));
}

int jv_obj_has(const JVal *obj, const char *key)
{
    return jv_obj_get(obj, key) != NULL;
}

int jv_is_ref(const JVal *v)
{
    if (!v || v->type != J_OBJ || v->u.o.n != 1)
        return 0;
    if (v->u.o.k[0].len != 4 || memcmp(v->u.o.k[0].s, "$ref", 4) != 0)
        return 0;
    return v->u.o.v[0]->type == J_STR;
}

/* ------------------------------------------------------------------ */
/* parser                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    const unsigned char *p;
    const unsigned char *end;
    char err[192];
    int depth;
} Parser;

static void perr(Parser *p, const char *msg)
{
    if (p->err[0] == '\0')
        snprintf(p->err, sizeof(p->err), "%s", msg);
}

static void skip_ws(Parser *p)
{
    while (p->p < p->end) {
        unsigned char c = *p->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            p->p++;
        else
            break;
    }
}

typedef struct {
    char *buf;
    size_t len, cap;
} StrBuf;

static void sb_put(StrBuf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        while (b->len + n + 1 > b->cap)
            b->cap = b->cap ? b->cap * 2 : 16;
        b->buf = xrealloc(b->buf, b->cap);
    }
    if (n)
        memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
}

static void sb_putc(StrBuf *b, char c) { sb_put(b, &c, 1); }

static int hexval(unsigned char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int parse_u4(Parser *p, unsigned *out)
{
    if (p->end - p->p < 4)
        return -1;
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexval(p->p[i]);
        if (h < 0)
            return -1;
        v = (v << 4) | (unsigned)h;
    }
    p->p += 4;
    *out = v;
    return 0;
}

static void sb_put_utf8(StrBuf *b, unsigned cp)
{
    char tmp[4];
    if (cp < 0x80) {
        tmp[0] = (char)cp;
        sb_put(b, tmp, 1);
    } else if (cp < 0x800) {
        tmp[0] = (char)(0xC0 | (cp >> 6));
        tmp[1] = (char)(0x80 | (cp & 0x3F));
        sb_put(b, tmp, 2);
    } else if (cp < 0x10000) {
        tmp[0] = (char)(0xE0 | (cp >> 12));
        tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[2] = (char)(0x80 | (cp & 0x3F));
        sb_put(b, tmp, 3);
    } else {
        tmp[0] = (char)(0xF0 | (cp >> 18));
        tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[3] = (char)(0x80 | (cp & 0x3F));
        sb_put(b, tmp, 4);
    }
}

/* Validate one UTF-8 sequence starting at p; returns length or 0. */
static size_t utf8_seq_len(const unsigned char *p, const unsigned char *end)
{
    unsigned char c = p[0];
    size_t n;
    unsigned cp;
    if (c < 0x80)
        return 1;
    if (c >= 0xC2 && c <= 0xDF) {
        n = 2;
        cp = c & 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
        n = 3;
        cp = c & 0x0F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        n = 4;
        cp = c & 0x07;
    } else {
        return 0;
    }
    if ((size_t)(end - p) < n)
        return 0;
    for (size_t i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80)
            return 0;
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    if (n == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)))
        return 0;
    if (n == 4 && (cp < 0x10000 || cp > 0x10FFFF))
        return 0;
    return n;
}

static int parse_string(Parser *p, JStr *out)
{
    if (p->p >= p->end || *p->p != '"') {
        perr(p, "expected string");
        return -1;
    }
    p->p++;
    StrBuf b = {0};
    for (;;) {
        if (p->p >= p->end) {
            perr(p, "unterminated string");
            free(b.buf);
            return -1;
        }
        unsigned char c = *p->p;
        if (c == '"') {
            p->p++;
            break;
        }
        if (c < 0x20) {
            perr(p, "control character in string");
            free(b.buf);
            return -1;
        }
        if (c == '\\') {
            p->p++;
            if (p->p >= p->end) {
                perr(p, "bad escape");
                free(b.buf);
                return -1;
            }
            unsigned char e = *p->p++;
            switch (e) {
            case '"': sb_putc(&b, '"'); break;
            case '\\': sb_putc(&b, '\\'); break;
            case '/': sb_putc(&b, '/'); break;
            case 'b': sb_putc(&b, '\b'); break;
            case 'f': sb_putc(&b, '\f'); break;
            case 'n': sb_putc(&b, '\n'); break;
            case 'r': sb_putc(&b, '\r'); break;
            case 't': sb_putc(&b, '\t'); break;
            case 'u': {
                unsigned cp;
                if (parse_u4(p, &cp) < 0) {
                    perr(p, "bad \\u escape");
                    free(b.buf);
                    return -1;
                }
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (p->end - p->p < 2 || p->p[0] != '\\' || p->p[1] != 'u') {
                        perr(p, "lone surrogate");
                        free(b.buf);
                        return -1;
                    }
                    p->p += 2;
                    unsigned lo;
                    if (parse_u4(p, &lo) < 0 || lo < 0xDC00 || lo > 0xDFFF) {
                        perr(p, "bad surrogate pair");
                        free(b.buf);
                        return -1;
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    perr(p, "lone surrogate");
                    free(b.buf);
                    return -1;
                }
                sb_put_utf8(&b, cp);
                break;
            }
            default:
                perr(p, "bad escape");
                free(b.buf);
                return -1;
            }
            continue;
        }
        if (c < 0x80) {
            sb_putc(&b, (char)c);
            p->p++;
        } else {
            size_t n = utf8_seq_len(p->p, p->end);
            if (n == 0) {
                perr(p, "invalid utf-8");
                free(b.buf);
                return -1;
            }
            sb_put(&b, (const char *)p->p, n);
            p->p += n;
        }
    }
    if (!b.buf) {
        b.buf = xmalloc(1);
        b.buf[0] = '\0';
    }
    out->s = b.buf;
    out->len = b.len;
    return 0;
}

static int parse_number(Parser *p, JVal **out)
{
    const unsigned char *start = p->p;
    int is_int_token = 1;

    if (p->p < p->end && *p->p == '-')
        p->p++;
    if (p->p >= p->end) {
        perr(p, "bad number");
        return -1;
    }
    if (*p->p == '0') {
        p->p++;
    } else if (*p->p >= '1' && *p->p <= '9') {
        while (p->p < p->end && *p->p >= '0' && *p->p <= '9')
            p->p++;
    } else {
        perr(p, "bad number");
        return -1;
    }
    if (p->p < p->end && *p->p == '.') {
        is_int_token = 0;
        p->p++;
        if (p->p >= p->end || *p->p < '0' || *p->p > '9') {
            perr(p, "bad number fraction");
            return -1;
        }
        while (p->p < p->end && *p->p >= '0' && *p->p <= '9')
            p->p++;
    }
    if (p->p < p->end && (*p->p == 'e' || *p->p == 'E')) {
        is_int_token = 0;
        p->p++;
        if (p->p < p->end && (*p->p == '+' || *p->p == '-'))
            p->p++;
        if (p->p >= p->end || *p->p < '0' || *p->p > '9') {
            perr(p, "bad number exponent");
            return -1;
        }
        while (p->p < p->end && *p->p >= '0' && *p->p <= '9')
            p->p++;
    }

    size_t len = (size_t)(p->p - start);
    char *tok = xstrndup((const char *)start, len);

    if (is_int_token) {
        const char *q = tok;
        int neg = 0;
        if (*q == '-') {
            neg = 1;
            q++;
        }
        uint64_t mag = 0;
        int overflow = 0;
        while (*q) {
            unsigned d = (unsigned)(*q - '0');
            if (mag > (UINT64_MAX - d) / 10) {
                overflow = 1;
                break;
            }
            mag = mag * 10 + d;
            q++;
        }
        if (!overflow) {
            if (!neg && mag <= (uint64_t)INT64_MAX) {
                free(tok);
                *out = jv_int((int64_t)mag);
                return 0;
            }
            if (neg) {
                uint64_t lim = (uint64_t)INT64_MAX + 1u;
                if (mag <= lim) {
                    int64_t val = (mag == lim) ? INT64_MIN : -(int64_t)mag;
                    free(tok);
                    *out = jv_int(val);
                    return 0;
                }
            }
        }
    }

    {
        char *endp = NULL;
        double d = strtod(tok, &endp);
        if (!endp || *endp != '\0' || !isfinite(d)) {
            perr(p, "number out of range");
            free(tok);
            return -1;
        }
        free(tok);
        *out = jv_double(d);
    }
    return 0;
}

static int parse_value(Parser *p, JVal **out);

static int parse_array(Parser *p, JVal **out)
{
    JVal *arr = jv_arr();
    p->p++; /* [ */
    skip_ws(p);
    if (p->p < p->end && *p->p == ']') {
        p->p++;
        *out = arr;
        return 0;
    }
    for (;;) {
        JVal *v = NULL;
        if (parse_value(p, &v) < 0)
            return -1;
        jv_arr_push(arr, v);
        skip_ws(p);
        if (p->p >= p->end) {
            perr(p, "unterminated array");
            return -1;
        }
        if (*p->p == ',') {
            p->p++;
            skip_ws(p);
            continue;
        }
        if (*p->p == ']') {
            p->p++;
            *out = arr;
            return 0;
        }
        perr(p, "expected ',' or ']'");
        return -1;
    }
}

static int parse_object(Parser *p, JVal **out)
{
    JVal *obj = jv_obj();
    p->p++; /* { */
    skip_ws(p);
    if (p->p < p->end && *p->p == '}') {
        p->p++;
        *out = obj;
        return 0;
    }
    for (;;) {
        JStr key;
        memset(&key, 0, sizeof(key));
        skip_ws(p);
        if (parse_string(p, &key) < 0)
            return -1;
        skip_ws(p);
        if (p->p >= p->end || *p->p != ':') {
            perr(p, "expected ':'");
            return -1;
        }
        p->p++;
        JVal *v = NULL;
        if (parse_value(p, &v) < 0)
            return -1;
        /* Duplicate keys are preserved; lookups use the first occurrence. */
        jv_obj_append(obj, key.s, key.len, v);
        free(key.s);
        skip_ws(p);
        if (p->p >= p->end) {
            perr(p, "unterminated object");
            return -1;
        }
        if (*p->p == ',') {
            p->p++;
            continue;
        }
        if (*p->p == '}') {
            p->p++;
            *out = obj;
            return 0;
        }
        perr(p, "expected ',' or '}'");
        return -1;
    }
}

static int parse_value(Parser *p, JVal **out)
{
    skip_ws(p);
    if (p->p >= p->end) {
        perr(p, "unexpected end of input");
        return -1;
    }
    if (p->depth++ > 512) {
        perr(p, "nesting too deep");
        return -1;
    }
    int rc = 0;
    unsigned char c = *p->p;
    switch (c) {
    case '{': rc = parse_object(p, out); break;
    case '[': rc = parse_array(p, out); break;
    case '"': {
        JStr s;
        memset(&s, 0, sizeof(s));
        if (parse_string(p, &s) < 0) {
            rc = -1;
        } else {
            *out = jv_str(s.s, s.len);
            free(s.s);
        }
        break;
    }
    case 't':
        if ((size_t)(p->end - p->p) >= 4 && memcmp(p->p, "true", 4) == 0) {
            p->p += 4;
            *out = jv_bool(1);
        } else {
            perr(p, "bad literal");
            rc = -1;
        }
        break;
    case 'f':
        if ((size_t)(p->end - p->p) >= 5 && memcmp(p->p, "false", 5) == 0) {
            p->p += 5;
            *out = jv_bool(0);
        } else {
            perr(p, "bad literal");
            rc = -1;
        }
        break;
    case 'n':
        if ((size_t)(p->end - p->p) >= 4 && memcmp(p->p, "null", 4) == 0) {
            p->p += 4;
            *out = jv_null();
        } else {
            perr(p, "bad literal");
            rc = -1;
        }
        break;
    default:
        if (c == '-' || (c >= '0' && c <= '9'))
            rc = parse_number(p, out);
        else {
            perr(p, "unexpected character");
            rc = -1;
        }
        break;
    }
    p->depth--;
    return rc;
}

int jv_parse(const char *buf, size_t len, JVal **out, char *err, size_t errsz)
{
    Parser p;
    memset(&p, 0, sizeof(p));
    p.p = (const unsigned char *)buf;
    p.end = p.p + len;
    JVal *v = NULL;
    if (parse_value(&p, &v) < 0) {
        if (err && errsz)
            snprintf(err, errsz, "%s", p.err[0] ? p.err : "parse error");
        return -1;
    }
    skip_ws(&p);
    if (p.p != p.end) {
        if (err && errsz)
            snprintf(err, errsz, "trailing data after JSON document");
        return -1;
    }
    *out = v;
    return 0;
}

/* ------------------------------------------------------------------ */
/* serializer                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    char *buf;
    size_t len, cap;
} Out;

static void out_put(Out *o, const char *s, size_t n)
{
    if (o->len + n + 1 > o->cap) {
        while (o->len + n + 1 > o->cap)
            o->cap = o->cap ? o->cap * 2 : 64;
        o->buf = xrealloc(o->buf, o->cap);
    }
    if (n)
        memcpy(o->buf + o->len, s, n);
    o->len += n;
    o->buf[o->len] = '\0';
}

static void out_cstr(Out *o, const char *s) { out_put(o, s, strlen(s)); }

static void out_string(Out *o, const char *s, size_t len)
{
    out_put(o, "\"", 1);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': out_put(o, "\\\"", 2); break;
        case '\\': out_put(o, "\\\\", 2); break;
        case '\b': out_put(o, "\\b", 2); break;
        case '\f': out_put(o, "\\f", 2); break;
        case '\n': out_put(o, "\\n", 2); break;
        case '\r': out_put(o, "\\r", 2); break;
        case '\t': out_put(o, "\\t", 2); break;
        default:
            if (c < 0x20) {
                char tmp[8];
                snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                out_put(o, tmp, 6);
            } else {
                out_put(o, (const char *)&s[i], 1);
            }
            break;
        }
    }
    out_put(o, "\"", 1);
}

static void out_number(Out *o, const JVal *v)
{
    char tmp[64];
    if (v->u.n.is_int) {
        snprintf(tmp, sizeof(tmp), "%lld", (long long)v->u.n.i);
        out_cstr(o, tmp);
        return;
    }
    if (!isfinite(v->u.n.d)) {
        out_cstr(o, "null");
        return;
    }
    snprintf(tmp, sizeof(tmp), "%.17g", v->u.n.d);
    /* "%.17g" can produce forms such as "1e+300" which are valid JSON. */
    out_cstr(o, tmp);
}

static void dump_value(Out *o, const JVal *v)
{
    if (!v) {
        out_cstr(o, "null");
        return;
    }
    switch (v->type) {
    case J_NULL: out_cstr(o, "null"); break;
    case J_BOOL: out_cstr(o, v->u.b ? "true" : "false"); break;
    case J_NUM: out_number(o, v); break;
    case J_STR: out_string(o, v->u.s.s, v->u.s.len); break;
    case J_ARR:
        out_put(o, "[", 1);
        for (size_t i = 0; i < v->u.a.n; i++) {
            if (i)
                out_put(o, ",", 1);
            dump_value(o, v->u.a.v[i]);
        }
        out_put(o, "]", 1);
        break;
    case J_OBJ:
        out_put(o, "{", 1);
        for (size_t i = 0; i < v->u.o.n; i++) {
            if (i)
                out_put(o, ",", 1);
            out_string(o, v->u.o.k[i].s, v->u.o.k[i].len);
            out_put(o, ":", 1);
            dump_value(o, v->u.o.v[i]);
        }
        out_put(o, "}", 1);
        break;
    }
}

char *jv_dump(const JVal *v, size_t *outlen)
{
    Out o = {0};
    dump_value(&o, v);
    if (!o.buf) {
        o.buf = xmalloc(1);
        o.buf[0] = '\0';
    }
    if (outlen)
        *outlen = o.len;
    return o.buf;
}

/* ------------------------------------------------------------------ */
/* comparison                                                          */
/* ------------------------------------------------------------------ */

const char *jv_type_name(const JVal *v)
{
    if (!v)
        return "null";
    switch (v->type) {
    case J_NULL: return "null";
    case J_BOOL: return "boolean";
    case J_NUM: return "number";
    case J_STR: return "string";
    case J_ARR: return "array";
    case J_OBJ: return "object";
    }
    return "null";
}

int jv_truthy(const JVal *v)
{
    if (!v)
        return 0;
    switch (v->type) {
    case J_NULL: return 0;
    case J_BOOL: return v->u.b != 0;
    case J_NUM:
        if (v->u.n.is_int)
            return v->u.n.i != 0;
        return v->u.n.d != 0.0;
    case J_STR: return v->u.s.len != 0;
    case J_ARR: return v->u.a.n != 0;
    case J_OBJ: return v->u.o.n != 0;
    }
    return 0;
}

static int cmp_str_bytes(const JStr *a, const JStr *b)
{
    size_t n = a->len < b->len ? a->len : b->len;
    int c = n ? memcmp(a->s, b->s, n) : 0;
    if (c)
        return c < 0 ? -1 : 1;
    if (a->len < b->len)
        return -1;
    if (a->len > b->len)
        return 1;
    return 0;
}

static int num_cmp(const JVal *a, const JVal *b)
{
    if (a->u.n.is_int && b->u.n.is_int) {
        if (a->u.n.i < b->u.n.i)
            return -1;
        if (a->u.n.i > b->u.n.i)
            return 1;
        return 0;
    }
    long double x = a->u.n.is_int ? (long double)a->u.n.i : (long double)a->u.n.d;
    long double y = b->u.n.is_int ? (long double)b->u.n.i : (long double)b->u.n.d;
    if (x < y)
        return -1;
    if (x > y)
        return 1;
    return 0;
}

static int type_rank(const JVal *v)
{
    switch (v->type) {
    case J_NUM: return 0;
    case J_STR: return 1;
    case J_BOOL: return 2;
    case J_NULL: return 3;
    case J_ARR: return 4;
    case J_OBJ: return 5;
    }
    return 6;
}

int jv_equal(const JVal *a, const JVal *b)
{
    if (!a || !b)
        return (!a && !b) || (a == b);
    if (a->type != b->type)
        return 0;
    switch (a->type) {
    case J_NULL: return 1;
    case J_BOOL: return (a->u.b != 0) == (b->u.b != 0);
    case J_NUM: return num_cmp(a, b) == 0;
    case J_STR:
        return a->u.s.len == b->u.s.len &&
               (a->u.s.len == 0 || memcmp(a->u.s.s, b->u.s.s, a->u.s.len) == 0);
    case J_ARR:
        if (a->u.a.n != b->u.a.n)
            return 0;
        for (size_t i = 0; i < a->u.a.n; i++)
            if (!jv_equal(a->u.a.v[i], b->u.a.v[i]))
                return 0;
        return 1;
    case J_OBJ:
        if (a->u.o.n != b->u.o.n)
            return 0;
        for (size_t i = 0; i < a->u.o.n; i++) {
            JVal *bv = jv_obj_get_len(b, a->u.o.k[i].s, a->u.o.k[i].len);
            if (!bv || !jv_equal(a->u.o.v[i], bv))
                return 0;
        }
        return 1;
    }
    return 0;
}

static int cmp_jstr_qsort(const void *pa, const void *pb)
{
    return cmp_str_bytes((const JStr *)pa, (const JStr *)pb);
}

int jv_compare(const JVal *a, const JVal *b)
{
    if (!a || !b) {
        int ra = a ? type_rank(a) : 3;
        int rb = b ? type_rank(b) : 3;
        if (ra != rb)
            return ra < rb ? -1 : 1;
        return 0;
    }
    int ra = type_rank(a), rb = type_rank(b);
    if (ra != rb)
        return ra < rb ? -1 : 1;
    switch (a->type) {
    case J_NULL:
        return 0;
    case J_BOOL: {
        int x = a->u.b != 0, y = b->u.b != 0;
        if (x == y)
            return 0;
        return x ? 1 : -1;
    }
    case J_NUM:
        return num_cmp(a, b);
    case J_STR:
        return cmp_str_bytes(&a->u.s, &b->u.s);
    case J_ARR: {
        size_t n = a->u.a.n < b->u.a.n ? a->u.a.n : b->u.a.n;
        for (size_t i = 0; i < n; i++) {
            int c = jv_compare(a->u.a.v[i], b->u.a.v[i]);
            if (c)
                return c;
        }
        if (a->u.a.n < b->u.a.n)
            return -1;
        if (a->u.a.n > b->u.a.n)
            return 1;
        return 0;
    }
    case J_OBJ: {
        JStr *ka = xmalloc((a->u.o.n ? a->u.o.n : 1) * sizeof(JStr));
        JStr *kb = xmalloc((b->u.o.n ? b->u.o.n : 1) * sizeof(JStr));
        memcpy(ka, a->u.o.k, a->u.o.n * sizeof(JStr));
        memcpy(kb, b->u.o.k, b->u.o.n * sizeof(JStr));
        if (a->u.o.n)
            qsort(ka, a->u.o.n, sizeof(JStr), cmp_jstr_qsort);
        if (b->u.o.n)
            qsort(kb, b->u.o.n, sizeof(JStr), cmp_jstr_qsort);
        size_t n = a->u.o.n < b->u.o.n ? a->u.o.n : b->u.o.n;
        int res = 0;
        for (size_t i = 0; i < n; i++) {
            int c = cmp_str_bytes(&ka[i], &kb[i]);
            if (c) {
                res = c;
                goto done;
            }
        }
        if (a->u.o.n != b->u.o.n) {
            res = a->u.o.n < b->u.o.n ? -1 : 1;
            goto done;
        }
        for (size_t i = 0; i < n; i++) {
            JVal *av = jv_obj_get_len(a, ka[i].s, ka[i].len);
            JVal *bv = jv_obj_get_len(b, kb[i].s, kb[i].len);
            int c = jv_compare(av, bv);
            if (c) {
                res = c;
                goto done;
            }
        }
    done:
        free(ka);
        free(kb);
        return res;
    }
    }
    return 0;
}
