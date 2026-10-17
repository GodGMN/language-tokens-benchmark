/*
 * workflow - declarative workflow engine (C11, no third-party dependencies)
 *
 * Implements the contract in ../../../SPEC.md
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <limits.h>
#include <ctype.h>

/* ------------------------------------------------------------------ */
/* Values                                                              */
/* ------------------------------------------------------------------ */

typedef enum { V_NULL, V_BOOL, V_NUM, V_STR, V_ARR, V_OBJ } VType;

typedef struct Value Value;

typedef struct {
    int is_int;           /* integer literal that fits i64 */
    int64_t i;
    double d;
    char *raw;            /* original lexeme, or NULL for computed numbers */
} Num;

typedef struct {
    char *s;
    size_t len;
} Str;

typedef struct {
    Value **items;
    size_t len, cap;
} Arr;

typedef struct {
    char *key;
    size_t klen;
    Value *val;
} Pair;

typedef struct {
    Pair *items;
    size_t len, cap;
} Obj;

struct Value {
    VType t;
    int b;
    Num num;
    Str str;
    Arr arr;
    Obj obj;
};

/* ---- growable byte buffer ---- */

typedef struct {
    char *p;
    size_t len, cap;
} Buf;

static void buf_grow(Buf *b, size_t extra)
{
    size_t need = b->len + extra;
    if (need < b->len) {
        fprintf(stderr, "workflow: buffer overflow\n");
        exit(3);
    }
    if (need <= b->cap)
        return;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < need) {
        if (cap > (size_t)-1 / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    char *np = realloc(b->p, cap);
    if (!np) {
        fprintf(stderr, "workflow: out of memory\n");
        exit(3);
    }
    b->p = np;
    b->cap = cap;
}

static void buf_put(Buf *b, const void *s, size_t n)
{
    if (n == 0)
        return;
    buf_grow(b, n);
    memcpy(b->p + b->len, s, n);
    b->len += n;
}

static void buf_putc(Buf *b, char c)
{
    buf_grow(b, 1);
    b->p[b->len++] = c;
}

static void buf_puts(Buf *b, const char *s)
{
    buf_put(b, s, strlen(s));
}

/* ---- constructors ---- */

static Value *v_new(VType t)
{
    Value *v = calloc(1, sizeof(Value));
    if (!v) {
        fprintf(stderr, "workflow: out of memory\n");
        exit(3);
    }
    v->t = t;
    return v;
}

static Value *v_null(void) { return v_new(V_NULL); }

static Value *v_bool(int b)
{
    Value *v = v_new(V_BOOL);
    v->b = b ? 1 : 0;
    return v;
}

static Value *v_i64(int64_t i)
{
    Value *v = v_new(V_NUM);
    v->num.is_int = 1;
    v->num.i = i;
    v->num.d = (double)i;
    return v;
}

static Value *v_str_n(const char *s, size_t len)
{
    Value *v = v_new(V_STR);
    char *p = malloc(len + 1);
    if (!p) {
        fprintf(stderr, "workflow: out of memory\n");
        exit(3);
    }
    if (len && s)
        memcpy(p, s, len);
    p[len] = '\0';
    v->str.s = p;
    v->str.len = len;
    return v;
}

static Value *v_str_c(const char *s) { return v_str_n(s, strlen(s)); }

static Value *v_arr(void) { return v_new(V_ARR); }

static void arr_push(Value *a, Value *v)
{
    if (a->arr.len == a->arr.cap) {
        size_t cap = a->arr.cap ? a->arr.cap * 2 : 4;
        Value **np = realloc(a->arr.items, cap * sizeof(Value *));
        if (!np) {
            fprintf(stderr, "workflow: out of memory\n");
            exit(3);
        }
        a->arr.items = np;
        a->arr.cap = cap;
    }
    a->arr.items[a->arr.len++] = v;
}

static Value *v_obj(void) { return v_new(V_OBJ); }

static int key_eq(const char *k, size_t klen, const char *lit)
{
    size_t l = strlen(lit);
    return klen == l && memcmp(k, lit, l) == 0;
}

static Value *obj_get_n(const Value *o, const char *key, size_t klen)
{
    if (!o || o->t != V_OBJ)
        return NULL;
    /* Duplicate keys are preserved by the parser; lookup follows the
       conventional last-one-wins rule. */
    Value *found = NULL;
    for (size_t i = 0; i < o->obj.len; i++) {
        if (o->obj.items[i].klen == klen &&
            memcmp(o->obj.items[i].key, key, klen) == 0)
            found = o->obj.items[i].val;
    }
    return found;
}

static Value *obj_get_c(const Value *o, const char *key)
{
    return obj_get_n(o, key, strlen(key));
}

static int obj_has(const Value *o, const char *key)
{
    return obj_get_c(o, key) != NULL;
}

static void obj_put_n(Value *o, const char *key, size_t klen, Value *val)
{
    for (size_t i = 0; i < o->obj.len; i++) {
        if (o->obj.items[i].klen == klen &&
            memcmp(o->obj.items[i].key, key, klen) == 0) {
            o->obj.items[i].val = val;
            return;
        }
    }
    if (o->obj.len == o->obj.cap) {
        size_t cap = o->obj.cap ? o->obj.cap * 2 : 4;
        Pair *np = realloc(o->obj.items, cap * sizeof(Pair));
        if (!np) {
            fprintf(stderr, "workflow: out of memory\n");
            exit(3);
        }
        o->obj.items = np;
        o->obj.cap = cap;
    }
    char *k = malloc(klen + 1);
    if (!k) {
        fprintf(stderr, "workflow: out of memory\n");
        exit(3);
    }
    if (klen)
        memcpy(k, key, klen);
    k[klen] = '\0';
    Pair *p = &o->obj.items[o->obj.len++];
    p->key = k;
    p->klen = klen;
    p->val = val;
}

/* Append unconditionally; used when rebuilding parsed values so that
   duplicate JSON keys survive resolution. */
static void obj_append_n(Value *o, const char *key, size_t klen, Value *val)
{
    if (o->obj.len == o->obj.cap) {
        size_t cap = o->obj.cap ? o->obj.cap * 2 : 4;
        Pair *np = realloc(o->obj.items, cap * sizeof(Pair));
        if (!np) {
            fprintf(stderr, "workflow: out of memory\n");
            exit(3);
        }
        o->obj.items = np;
        o->obj.cap = cap;
    }
    char *k = malloc(klen + 1);
    if (!k) {
        fprintf(stderr, "workflow: out of memory\n");
        exit(3);
    }
    if (klen)
        memcpy(k, key, klen);
    k[klen] = '\0';
    Pair *p = &o->obj.items[o->obj.len++];
    p->key = k;
    p->klen = klen;
    p->val = val;
}

static void obj_put_c(Value *o, const char *key, Value *val)
{
    obj_put_n(o, key, strlen(key), val);
}

/* ------------------------------------------------------------------ */
/* JSON parser                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *s;
    size_t len, pos;
    char err[256];
} P;

static void perr(P *p, const char *msg)
{
    if (p->err[0] == '\0')
        snprintf(p->err, sizeof p->err, "%s (at offset %zu)", msg, p->pos);
}

static void skip_ws(P *p)
{
    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            p->pos++;
        else
            break;
    }
}

static int hexv(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int parse_hex4(P *p, unsigned *out)
{
    if (p->pos + 4 > p->len) {
        perr(p, "truncated \\u escape");
        return 0;
    }
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexv(p->s[p->pos + i]);
        if (h < 0) {
            perr(p, "bad \\u escape");
            return 0;
        }
        v = v * 16u + (unsigned)h;
    }
    p->pos += 4;
    *out = v;
    return 1;
}

static void buf_put_utf8(Buf *b, unsigned cp)
{
    if (cp < 0x80) {
        buf_putc(b, (char)cp);
    } else if (cp < 0x800) {
        buf_putc(b, (char)(0xC0 | (cp >> 6)));
        buf_putc(b, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        buf_putc(b, (char)(0xE0 | (cp >> 12)));
        buf_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        buf_putc(b, (char)(0x80 | (cp & 0x3F)));
    } else {
        buf_putc(b, (char)(0xF0 | (cp >> 18)));
        buf_putc(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
        buf_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        buf_putc(b, (char)(0x80 | (cp & 0x3F)));
    }
}

static Value *parse_string(P *p)
{
    p->pos++; /* opening quote */
    Buf b = {0};
    for (;;) {
        if (p->pos >= p->len) {
            perr(p, "unterminated string");
            free(b.p);
            return NULL;
        }
        unsigned char c = (unsigned char)p->s[p->pos++];
        if (c == '"')
            break;
        if (c == '\\') {
            if (p->pos >= p->len) {
                perr(p, "truncated escape");
                free(b.p);
                return NULL;
            }
            char e = p->s[p->pos++];
            switch (e) {
            case '"': buf_putc(&b, '"'); break;
            case '\\': buf_putc(&b, '\\'); break;
            case '/': buf_putc(&b, '/'); break;
            case 'b': buf_putc(&b, '\b'); break;
            case 'f': buf_putc(&b, '\f'); break;
            case 'n': buf_putc(&b, '\n'); break;
            case 'r': buf_putc(&b, '\r'); break;
            case 't': buf_putc(&b, '\t'); break;
            case 'u': {
                unsigned u1;
                if (!parse_hex4(p, &u1)) {
                    free(b.p);
                    return NULL;
                }
                unsigned cp = u1;
                if (u1 >= 0xD800 && u1 <= 0xDBFF) {
                    if (p->pos + 1 < p->len && p->s[p->pos] == '\\' &&
                        p->s[p->pos + 1] == 'u') {
                        size_t save = p->pos;
                        p->pos += 2;
                        unsigned u2;
                        if (!parse_hex4(p, &u2)) {
                            free(b.p);
                            return NULL;
                        }
                        if (u2 >= 0xDC00 && u2 <= 0xDFFF) {
                            cp = 0x10000u + ((u1 - 0xD800u) << 10) +
                                 (u2 - 0xDC00u);
                        } else {
                            cp = 0xFFFDu;
                            p->pos = save;
                        }
                    } else {
                        cp = 0xFFFDu;
                    }
                } else if (u1 >= 0xDC00 && u1 <= 0xDFFF) {
                    cp = 0xFFFDu;
                }
                buf_put_utf8(&b, cp);
                break;
            }
            default:
                perr(p, "invalid escape");
                free(b.p);
                return NULL;
            }
        } else if (c < 0x20) {
            perr(p, "control character in string");
            free(b.p);
            return NULL;
        } else {
            buf_putc(&b, (char)c);
        }
    }
    Value *v = v_str_n(b.p ? b.p : "", b.len);
    free(b.p);
    return v;
}

static Value *parse_number(P *p)
{
    size_t start = p->pos;
    int is_int = 1;
    int neg = 0;

    if (p->s[p->pos] == '-') {
        neg = 1;
        p->pos++;
    }
    if (p->pos >= p->len) {
        perr(p, "truncated number");
        return NULL;
    }
    if (p->s[p->pos] == '0') {
        p->pos++;
        if (p->pos < p->len && p->s[p->pos] >= '0' && p->s[p->pos] <= '9') {
            perr(p, "leading zero in number");
            return NULL;
        }
    } else if (p->s[p->pos] >= '1' && p->s[p->pos] <= '9') {
        while (p->pos < p->len && p->s[p->pos] >= '0' &&
               p->s[p->pos] <= '9')
            p->pos++;
    } else {
        perr(p, "invalid number");
        return NULL;
    }
    if (p->pos < p->len && p->s[p->pos] == '.') {
        is_int = 0;
        p->pos++;
        if (p->pos >= p->len || p->s[p->pos] < '0' || p->s[p->pos] > '9') {
            perr(p, "invalid fraction");
            return NULL;
        }
        while (p->pos < p->len && p->s[p->pos] >= '0' &&
               p->s[p->pos] <= '9')
            p->pos++;
    }
    if (p->pos < p->len && (p->s[p->pos] == 'e' || p->s[p->pos] == 'E')) {
        is_int = 0;
        p->pos++;
        if (p->pos < p->len && (p->s[p->pos] == '+' || p->s[p->pos] == '-'))
            p->pos++;
        if (p->pos >= p->len || p->s[p->pos] < '0' || p->s[p->pos] > '9') {
            perr(p, "invalid exponent");
            return NULL;
        }
        while (p->pos < p->len && p->s[p->pos] >= '0' &&
               p->s[p->pos] <= '9')
            p->pos++;
    }

    size_t end = p->pos;
    char *raw = malloc(end - start + 1);
    if (!raw) {
        fprintf(stderr, "workflow: out of memory\n");
        exit(3);
    }
    memcpy(raw, p->s + start, end - start);
    raw[end - start] = '\0';

    Value *v = v_new(V_NUM);
    v->num.raw = raw;
    v->num.d = strtod(raw, NULL);

    if (is_int) {
        size_t i = neg ? 1 : 0;
        unsigned long long mag = 0;
        int ovf = 0;
        for (; raw[i]; i++) {
            unsigned dgt = (unsigned)(raw[i] - '0');
            if (mag > (ULLONG_MAX - dgt) / 10) {
                ovf = 1;
                break;
            }
            mag = mag * 10 + dgt;
        }
        unsigned long long limit =
            neg ? 9223372036854775808ULL : 9223372036854775807ULL;
        if (!ovf && mag <= limit) {
            v->num.is_int = 1;
            if (neg)
                v->num.i = (mag == 9223372036854775808ULL)
                               ? INT64_MIN
                               : -(int64_t)mag;
            else
                v->num.i = (int64_t)mag;
        }
    }
    return v;
}

static Value *parse_value(P *p, int depth);

static int match_lit(P *p, const char *lit)
{
    size_t l = strlen(lit);
    if (p->pos + l > p->len)
        return 0;
    if (memcmp(p->s + p->pos, lit, l) != 0)
        return 0;
    p->pos += l;
    return 1;
}

static Value *parse_array(P *p, int depth)
{
    p->pos++; /* '[' */
    Value *a = v_arr();
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == ']') {
        p->pos++;
        return a;
    }
    for (;;) {
        Value *e = parse_value(p, depth + 1);
        if (!e)
            return NULL;
        arr_push(a, e);
        skip_ws(p);
        if (p->pos >= p->len) {
            perr(p, "unterminated array");
            return NULL;
        }
        if (p->s[p->pos] == ',') {
            p->pos++;
            continue;
        }
        if (p->s[p->pos] == ']') {
            p->pos++;
            break;
        }
        perr(p, "expected ',' or ']' in array");
        return NULL;
    }
    return a;
}

static Value *parse_object(P *p, int depth)
{
    p->pos++; /* '{' */
    Value *o = v_obj();
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == '}') {
        p->pos++;
        return o;
    }
    for (;;) {
        skip_ws(p);
        if (p->pos >= p->len || p->s[p->pos] != '"') {
            perr(p, "expected object key string");
            return NULL;
        }
        Value *k = parse_string(p);
        if (!k)
            return NULL;
        skip_ws(p);
        if (p->pos >= p->len || p->s[p->pos] != ':') {
            perr(p, "expected ':' after object key");
            return NULL;
        }
        p->pos++;
        Value *v = parse_value(p, depth + 1);
        if (!v)
            return NULL;
        /* Preserve duplicate keys (needed to disambiguate the documented
           `type_is` predicate form); lookups use the last occurrence. */
        obj_append_n(o, k->str.s, k->str.len, v);
        skip_ws(p);
        if (p->pos >= p->len) {
            perr(p, "unterminated object");
            return NULL;
        }
        if (p->s[p->pos] == ',') {
            p->pos++;
            continue;
        }
        if (p->s[p->pos] == '}') {
            p->pos++;
            break;
        }
        perr(p, "expected ',' or '}' in object");
        return NULL;
    }
    return o;
}

static Value *parse_value(P *p, int depth)
{
    if (depth > 512) {
        perr(p, "document nested too deeply");
        return NULL;
    }
    skip_ws(p);
    if (p->pos >= p->len) {
        perr(p, "unexpected end of input");
        return NULL;
    }
    char c = p->s[p->pos];
    switch (c) {
    case 'n':
        if (!match_lit(p, "null")) {
            perr(p, "invalid literal");
            return NULL;
        }
        return v_null();
    case 't':
        if (!match_lit(p, "true")) {
            perr(p, "invalid literal");
            return NULL;
        }
        return v_bool(1);
    case 'f':
        if (!match_lit(p, "false")) {
            perr(p, "invalid literal");
            return NULL;
        }
        return v_bool(0);
    case '"':
        return parse_string(p);
    case '[':
        return parse_array(p, depth);
    case '{':
        return parse_object(p, depth);
    default:
        if (c == '-' || (c >= '0' && c <= '9'))
            return parse_number(p);
        perr(p, "unexpected character");
        return NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Serializer                                                          */
/* ------------------------------------------------------------------ */

static void ser_value(Buf *b, const Value *v);

static void ser_string(Buf *b, const char *s, size_t len)
{
    buf_putc(b, '"');
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': buf_puts(b, "\\\""); break;
        case '\\': buf_puts(b, "\\\\"); break;
        case '\b': buf_puts(b, "\\b"); break;
        case '\f': buf_puts(b, "\\f"); break;
        case '\n': buf_puts(b, "\\n"); break;
        case '\r': buf_puts(b, "\\r"); break;
        case '\t': buf_puts(b, "\\t"); break;
        default:
            if (c < 0x20) {
                char t[8];
                snprintf(t, sizeof t, "\\u%04x", c);
                buf_puts(b, t);
            } else {
                buf_putc(b, (char)c);
            }
        }
    }
    buf_putc(b, '"');
}

static void ser_value(Buf *b, const Value *v)
{
    if (!v) {
        buf_puts(b, "null");
        return;
    }
    switch (v->t) {
    case V_NULL:
        buf_puts(b, "null");
        break;
    case V_BOOL:
        buf_puts(b, v->b ? "true" : "false");
        break;
    case V_NUM:
        if (v->num.raw) {
            buf_puts(b, v->num.raw);
        } else {
            char t[32];
            snprintf(t, sizeof t, "%" PRId64, v->num.i);
            buf_puts(b, t);
        }
        break;
    case V_STR:
        ser_string(b, v->str.s, v->str.len);
        break;
    case V_ARR:
        buf_putc(b, '[');
        for (size_t i = 0; i < v->arr.len; i++) {
            if (i)
                buf_putc(b, ',');
            ser_value(b, v->arr.items[i]);
        }
        buf_putc(b, ']');
        break;
    case V_OBJ:
        buf_putc(b, '{');
        for (size_t i = 0; i < v->obj.len; i++) {
            if (i)
                buf_putc(b, ',');
            ser_string(b, v->obj.items[i].key, v->obj.items[i].klen);
            buf_putc(b, ':');
            ser_value(b, v->obj.items[i].val);
        }
        buf_putc(b, '}');
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Comparisions / equality                                             */
/* ------------------------------------------------------------------ */

static int cmp_bytes(const char *a, size_t al, const char *b, size_t bl)
{
    size_t n = al < bl ? al : bl;
    int c = n ? memcmp(a, b, n) : 0;
    if (c)
        return c < 0 ? -1 : 1;
    if (al == bl)
        return 0;
    return al < bl ? -1 : 1;
}

static int num_cmp(const Value *a, const Value *b)
{
    if (a->num.is_int && b->num.is_int) {
        if (a->num.i < b->num.i)
            return -1;
        if (a->num.i > b->num.i)
            return 1;
        return 0;
    }
    double x = a->num.is_int ? (double)a->num.i : a->num.d;
    double y = b->num.is_int ? (double)b->num.i : b->num.d;
    if (x < y)
        return -1;
    if (x > y)
        return 1;
    return 0;
}

static int equals_value(const Value *a, const Value *b)
{
    if (!a || !b)
        return 0;
    if (a->t != b->t)
        return 0;
    switch (a->t) {
    case V_NULL:
        return 1;
    case V_BOOL:
        return (a->b != 0) == (b->b != 0);
    case V_NUM:
        return num_cmp(a, b) == 0;
    case V_STR:
        return cmp_bytes(a->str.s, a->str.len, b->str.s, b->str.len) == 0;
    case V_ARR:
        if (a->arr.len != b->arr.len)
            return 0;
        for (size_t i = 0; i < a->arr.len; i++)
            if (!equals_value(a->arr.items[i], b->arr.items[i]))
                return 0;
        return 1;
    case V_OBJ:
        if (a->obj.len != b->obj.len)
            return 0;
        for (size_t i = 0; i < a->obj.len; i++) {
            Value *bv = obj_get_n(b, a->obj.items[i].key,
                                  a->obj.items[i].klen);
            if (!bv || !equals_value(a->obj.items[i].val, bv))
                return 0;
        }
        return 1;
    }
    return 0;
}

static int type_rank(VType t)
{
    switch (t) {
    case V_NUM: return 0;
    case V_STR: return 1;
    case V_BOOL: return 2;
    case V_NULL: return 3;
    case V_ARR: return 4;
    case V_OBJ: return 5;
    }
    return 6;
}

static int pair_ptr_cmp(const void *x, const void *y)
{
    const Pair *a = *(const Pair *const *)x;
    const Pair *b = *(const Pair *const *)y;
    return cmp_bytes(a->key, a->klen, b->key, b->klen);
}

/* sorted pointers to object's pairs; caller leaks (process is short-lived) */
static Pair **sorted_pairs(const Value *o, size_t *outn)
{
    size_t n = o->obj.len;
    Pair **arr = NULL;
    if (n) {
        arr = malloc(n * sizeof(Pair *));
        if (!arr) {
            fprintf(stderr, "workflow: out of memory\n");
            exit(3);
        }
        for (size_t i = 0; i < n; i++)
            arr[i] = &o->obj.items[i];
        qsort(arr, n, sizeof(Pair *), pair_ptr_cmp);
    }
    *outn = n;
    return arr;
}

static int total_cmp(const Value *a, const Value *b)
{
    int ra = type_rank(a->t), rb = type_rank(b->t);
    if (ra != rb)
        return ra < rb ? -1 : 1;
    switch (a->t) {
    case V_NULL:
        return 0;
    case V_BOOL:
        if (a->b == b->b)
            return 0;
        return a->b ? 1 : -1;
    case V_NUM:
        return num_cmp(a, b);
    case V_STR:
        return cmp_bytes(a->str.s, a->str.len, b->str.s, b->str.len);
    case V_ARR: {
        size_t n = a->arr.len < b->arr.len ? a->arr.len : b->arr.len;
        for (size_t i = 0; i < n; i++) {
            int c = total_cmp(a->arr.items[i], b->arr.items[i]);
            if (c)
                return c;
        }
        if (a->arr.len == b->arr.len)
            return 0;
        return a->arr.len < b->arr.len ? -1 : 1;
    }
    case V_OBJ: {
        size_t na, nb;
        Pair **pa = sorted_pairs(a, &na);
        Pair **pb = sorted_pairs(b, &nb);
        size_t n = na < nb ? na : nb;
        for (size_t i = 0; i < n; i++) {
            int c = cmp_bytes(pa[i]->key, pa[i]->klen, pb[i]->key,
                              pb[i]->klen);
            if (c)
                return c;
        }
        if (na != nb)
            return na < nb ? -1 : 1;
        for (size_t i = 0; i < n; i++) {
            int c = total_cmp(pa[i]->val, pb[i]->val);
            if (c)
                return c;
        }
        return 0;
    }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Structural hashing (used by unique)                                 */
/* ------------------------------------------------------------------ */

static uint64_t hash_mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}

static uint64_t hash_bytes(uint64_t h, const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++)
        h = (h ^ (unsigned char)s[i]) * 1099511628211ULL;
    return h;
}

static uint64_t hash_value(const Value *v)
{
    uint64_t h = 1469598103934665603ULL;
    h = hash_mix(h, (uint64_t)v->t);
    switch (v->t) {
    case V_NULL:
        return h;
    case V_BOOL:
        return hash_mix(h, v->b ? 1 : 2);
    case V_NUM: {
        /* Equal numbers (1 and 1.0) must hash identically. */
        double d = v->num.is_int ? (double)v->num.i : v->num.d;
        uint64_t bits;
        memcpy(&bits, &d, sizeof bits);
        return hash_mix(h, bits);
    }
    case V_STR:
        return hash_bytes(h, v->str.s, v->str.len);
    case V_ARR:
        for (size_t i = 0; i < v->arr.len; i++)
            h = hash_mix(h, hash_value(v->arr.items[i]));
        return hash_mix(h, v->arr.len);
    case V_OBJ: {
        /* key order is irrelevant for equality, so hash in sorted order */
        size_t n;
        Pair **sp = sorted_pairs(v, &n);
        for (size_t i = 0; i < n; i++) {
            h = hash_bytes(h, sp[i]->key, sp[i]->klen);
            h = hash_mix(h, hash_value(sp[i]->val));
        }
        free(sp);
        return hash_mix(h, n);
    }
    }
    return h;
}

static int truthy(const Value *v)
{
    if (!v)
        return 0;
    switch (v->t) {
    case V_NULL:
        return 0;
    case V_BOOL:
        return v->b != 0;
    case V_NUM:
        return v->num.is_int ? (v->num.i != 0) : (v->num.d != 0.0);
    case V_STR:
        return v->str.len > 0;
    case V_ARR:
        return v->arr.len > 0;
    case V_OBJ:
        return v->obj.len > 0;
    }
    return 0;
}

static const char *type_name(const Value *v)
{
    switch (v->t) {
    case V_NULL: return "null";
    case V_BOOL: return "boolean";
    case V_NUM: return "number";
    case V_STR: return "string";
    case V_ARR: return "array";
    case V_OBJ: return "object";
    }
    return "null";
}

/* ------------------------------------------------------------------ */
/* Errors                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *code;
    const char *step;
    char msg[256];
} Err;

static void err_set(Err *e, const char *code, const char *step,
                    const char *msg)
{
    e->code = code;
    e->step = step;
    if (msg)
        snprintf(e->msg, sizeof e->msg, "%s", msg);
    else
        e->msg[0] = '\0';
}

/* ------------------------------------------------------------------ */
/* Validation                                                          */
/* ------------------------------------------------------------------ */

static int is_known_op(const char *op)
{
    static const char *ops[] = {
        "add", "subtract", "multiply", "divide", "mod", "sum", "min",
        "max", "range", "concat", "split", "join", "uppercase",
        "lowercase", "trim", "replace", "length", "map", "filter",
        "reduce", "sort", "reverse", "unique", "slice", "flatten", "get",
        "set", "keys", "values", "merge", "pick", "omit", "constant",
        "equals", "compare", "and", "or", "not", "type", "fail_until"
    };
    for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++)
        if (strcmp(op, ops[i]) == 0)
            return 1;
    return 0;
}

static int id_ok(const char *s)
{
    if (!s || !(*s))
        return 0;
    unsigned char c = (unsigned char)*s;
    if (!(isalpha(c) || c == '_'))
        return 0;
    for (s++; *s; s++) {
        c = (unsigned char)*s;
        if (!(isalnum(c) || c == '_'))
            return 0;
    }
    return 1;
}

static int obj_only_keys(const Value *o, const char *const *allowed,
                         size_t n)
{
    for (size_t i = 0; i < o->obj.len; i++) {
        int ok = 0;
        for (size_t j = 0; j < n; j++) {
            if (key_eq(o->obj.items[i].key, o->obj.items[i].klen,
                       allowed[j])) {
                ok = 1;
                break;
            }
        }
        if (!ok)
            return 0;
    }
    return 1;
}

static int cond_shape_ok(const Value *c)
{
    if (!c || c->t != V_OBJ)
        return 0;
    Value *opv = obj_get_c(c, "op");
    if (!opv || opv->t != V_STR)
        return 0;
    const char *op = opv->str.s;

    static const char *bin[] = {"op", "left", "right"};
    static const char *cmpd[] = {"op", "conditions"};
    static const char *nt[] = {"op", "condition"};

    if (strcmp(op, "eq") == 0 || strcmp(op, "ne") == 0 ||
        strcmp(op, "gt") == 0 || strcmp(op, "gte") == 0 ||
        strcmp(op, "lt") == 0 || strcmp(op, "lte") == 0) {
        if (!obj_only_keys(c, bin, 3))
            return 0;
        if (!obj_has(c, "left") || !obj_has(c, "right"))
            return 0;
        return 1;
    }
    if (strcmp(op, "all") == 0 || strcmp(op, "any") == 0) {
        if (!obj_only_keys(c, cmpd, 2))
            return 0;
        Value *cs = obj_get_c(c, "conditions");
        if (!cs || cs->t != V_ARR)
            return 0;
        for (size_t i = 0; i < cs->arr.len; i++)
            if (!cond_shape_ok(cs->arr.items[i]))
                return 0;
        return 1;
    }
    if (strcmp(op, "not") == 0) {
        if (!obj_only_keys(c, nt, 2))
            return 0;
        Value *inner = obj_get_c(c, "condition");
        return cond_shape_ok(inner);
    }
    return 0;
}

typedef struct Step {
    char *id;              /* raw id string if the field was a string */
    Value *args;
    Value *when;
    char *op;
    char **deps;
    size_t *depidx;
    size_t ndeps;
    size_t index;
    int max_attempts;
    /* runtime state */
    int done;
    int skipped;
    Value *result;
    int64_t fail_attempts;
} Step;

static int validate_document(const Value *doc, Step **out_steps, size_t *outn,
                             Err *err)
{
    static const char *top_keys[] = {"version", "steps"};
    static const char *step_keys[] = {"id", "op", "args", "depends_on",
                                      "when", "retry"};
    static const char *retry_keys[] = {"max_attempts"};

    if (!doc || doc->t != V_OBJ) {
        err_set(err, "INVALID_WORKFLOW", NULL,
                "workflow document must be a JSON object");
        return 0;
    }
    Value *ver = obj_get_c(doc, "version");
    if (!ver || ver->t != V_NUM || !ver->num.is_int || ver->num.i != 1) {
        err_set(err, "INVALID_WORKFLOW", NULL,
                "version must be the integer literal 1");
        return 0;
    }
    Value *steps = obj_get_c(doc, "steps");
    if (!steps || steps->t != V_ARR) {
        err_set(err, "INVALID_WORKFLOW", NULL,
                "steps must be an array");
        return 0;
    }
    if (!obj_only_keys(doc, top_keys, 2)) {
        err_set(err, "INVALID_WORKFLOW", NULL,
                "unknown top-level field");
        return 0;
    }

    size_t n = steps->arr.len;
    Step *st = calloc(n ? n : 1, sizeof(Step));
    if (!st) {
        fprintf(stderr, "workflow: out of memory\n");
        exit(3);
    }

    for (size_t i = 0; i < n; i++) {
        Value *sv = steps->arr.items[i];
        if (!sv || sv->t != V_OBJ) {
            err_set(err, "INVALID_WORKFLOW", NULL,
                    "every step must be an object");
            return 0;
        }
        Step *s = &st[i];
        s->index = i;
        s->max_attempts = 1;

        Value *idv = obj_get_c(sv, "id");
        if (idv && idv->t == V_STR)
            s->id = idv->str.s;
        else
            s->id = NULL;
        const char *estep = s->id;

        if (!obj_only_keys(sv, step_keys, 6)) {
            err_set(err, "INVALID_WORKFLOW", estep,
                    "unknown step field");
            return 0;
        }
        if (!idv || idv->t != V_STR) {
            err_set(err, "INVALID_WORKFLOW", NULL,
                    "step id is required and must be a string");
            return 0;
        }
        if (!id_ok(s->id)) {
            err_set(err, "INVALID_WORKFLOW", estep,
                    "step id has an invalid format");
            return 0;
        }
        for (size_t j = 0; j < i; j++) {
            if (st[j].id && strcmp(st[j].id, s->id) == 0) {
                err_set(err, "INVALID_WORKFLOW", estep,
                        "duplicate step id");
                return 0;
            }
        }

        Value *opv = obj_get_c(sv, "op");
        if (!opv || opv->t != V_STR) {
            err_set(err, "INVALID_WORKFLOW", estep,
                    "step op is required and must be a string");
            return 0;
        }
        if (!is_known_op(opv->str.s)) {
            err_set(err, "INVALID_WORKFLOW", estep, "unknown operation");
            return 0;
        }
        s->op = opv->str.s;

        Value *av = obj_get_c(sv, "args");
        if (!av) {
            s->args = v_obj();
        } else if (av->t != V_OBJ) {
            err_set(err, "INVALID_WORKFLOW", estep,
                    "args must be an object");
            return 0;
        } else {
            s->args = av;
        }

        Value *dv = obj_get_c(sv, "depends_on");
        if (dv) {
            if (dv->t != V_ARR) {
                err_set(err, "INVALID_WORKFLOW", estep,
                        "depends_on must be an array");
                return 0;
            }
            s->ndeps = dv->arr.len;
            s->deps = calloc(s->ndeps ? s->ndeps : 1, sizeof(char *));
            s->depidx = calloc(s->ndeps ? s->ndeps : 1, sizeof(size_t));
            if (!s->deps || !s->depidx) {
                fprintf(stderr, "workflow: out of memory\n");
                exit(3);
            }
            for (size_t k = 0; k < s->ndeps; k++) {
                Value *e = dv->arr.items[k];
                if (!e || e->t != V_STR) {
                    err_set(err, "INVALID_WORKFLOW", estep,
                            "depends_on entries must be strings");
                    return 0;
                }
                s->deps[k] = e->str.s;
            }
        }

        Value *wv = obj_get_c(sv, "when");
        if (wv) {
            if (!cond_shape_ok(wv)) {
                err_set(err, "INVALID_WORKFLOW", estep,
                        "invalid condition shape");
                return 0;
            }
            s->when = wv;
        }

        Value *rv = obj_get_c(sv, "retry");
        if (rv) {
            if (rv->t != V_OBJ || !obj_only_keys(rv, retry_keys, 1)) {
                err_set(err, "INVALID_WORKFLOW", estep,
                        "invalid retry object");
                return 0;
            }
            Value *mav = obj_get_c(rv, "max_attempts");
            if (!mav || mav->t != V_NUM || !mav->num.is_int ||
                mav->num.i < 1 || mav->num.i > 5) {
                err_set(err, "INVALID_WORKFLOW", estep,
                        "retry.max_attempts must be an integer in 1..5");
                return 0;
            }
            s->max_attempts = (int)mav->num.i;
        }
    }

    /* dependency existence */
    for (size_t i = 0; i < n; i++) {
        for (size_t k = 0; k < st[i].ndeps; k++) {
            size_t j;
            int found = 0;
            for (j = 0; j < n; j++) {
                if (st[j].id && strcmp(st[j].id, st[i].deps[k]) == 0) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                err_set(err, "UNKNOWN_DEPENDENCY", st[i].id,
                        "depends_on references an unknown step");
                return 0;
            }
            st[i].depidx[k] = j;
        }
    }

    /* cycle detection (DFS) */
    {
        int *state = calloc(n ? n : 1, sizeof(int));
        if (!state) {
            fprintf(stderr, "workflow: out of memory\n");
            exit(3);
        }
        /* iterative DFS to avoid deep recursion */
        for (size_t start = 0; start < n; start++) {
            if (state[start] != 0)
                continue;
            size_t *stack = malloc((n ? n : 1) * sizeof(size_t));
            size_t *next = malloc((n ? n : 1) * sizeof(size_t));
            if (!stack || !next) {
                fprintf(stderr, "workflow: out of memory\n");
                exit(3);
            }
            size_t sp = 0;
            stack[sp] = start;
            next[sp] = 0;
            state[start] = 1;
            sp++;
            while (sp > 0) {
                size_t u = stack[sp - 1];
                if (next[sp - 1] < st[u].ndeps) {
                    size_t v = st[u].depidx[next[sp - 1]++];
                    if (state[v] == 1) {
                        err_set(err, "CYCLIC_DEPENDENCY", NULL,
                                "dependency cycle detected");
                        free(stack);
                        free(next);
                        free(state);
                        return 0;
                    }
                    if (state[v] == 0) {
                        state[v] = 1;
                        stack[sp] = v;
                        next[sp] = 0;
                        sp++;
                    }
                } else {
                    state[u] = 2;
                    sp--;
                }
            }
            free(stack);
            free(next);
        }
        free(state);
    }

    *out_steps = st;
    *outn = n;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Reference resolution                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    Step *steps;
    size_t n;
    unsigned char *reach; /* n*n transitive closure */
    size_t *index;        /* unused, kept for clarity */
} Ctx;

static size_t find_step(Ctx *ctx, const char *id, size_t len)
{
    for (size_t i = 0; i < ctx->n; i++) {
        const char *sid = ctx->steps[i].id;
        if (sid && strlen(sid) == len && memcmp(sid, id, len) == 0)
            return i;
    }
    return (size_t)-1;
}

static int resolve_ref(const Value *rv, Ctx *ctx, Step *self, Value **out,
                       Err *err);

static int resolve_value(const Value *v, Ctx *ctx, Step *self, Value **out,
                         Err *err, int depth)
{
    if (depth > 2048) {
        err_set(err, "INVALID_REFERENCE", self->id,
                "reference/argument nesting too deep");
        return 0;
    }
    if (!v) {
        *out = v_null();
        return 1;
    }
    if (v->t == V_OBJ) {
        if (v->obj.len == 1 &&
            key_eq(v->obj.items[0].key, v->obj.items[0].klen, "$ref") &&
            v->obj.items[0].val && v->obj.items[0].val->t == V_STR) {
            return resolve_ref(v->obj.items[0].val, ctx, self, out, err);
        }
        Value *o = v_obj();
        for (size_t i = 0; i < v->obj.len; i++) {
            Value *tmp;
            if (!resolve_value(v->obj.items[i].val, ctx, self, &tmp, err,
                               depth + 1))
                return 0;
            obj_append_n(o, v->obj.items[i].key, v->obj.items[i].klen, tmp);
        }
        *out = o;
        return 1;
    }
    if (v->t == V_ARR) {
        Value *a = v_arr();
        for (size_t i = 0; i < v->arr.len; i++) {
            Value *tmp;
            if (!resolve_value(v->arr.items[i], ctx, self, &tmp, err,
                               depth + 1))
                return 0;
            arr_push(a, tmp);
        }
        *out = a;
        return 1;
    }
    *out = (Value *)v;
    return 1;
}

static int resolve_ref(const Value *rv, Ctx *ctx, Step *self, Value **out,
                       Err *err)
{
    const char *t = rv->str.s;
    size_t tlen = rv->str.len;

    size_t dot = tlen;
    for (size_t i = 0; i < tlen; i++) {
        if (t[i] == '.') {
            dot = i;
            break;
        }
    }
    size_t idlen = dot;
    if (idlen == 0) {
        err_set(err, "INVALID_REFERENCE", self->id,
                "empty reference target");
        return 0;
    }
    size_t ti = find_step(ctx, t, idlen);
    if (ti == (size_t)-1) {
        err_set(err, "INVALID_REFERENCE", self->id,
                "reference to unknown step");
        return 0;
    }
    if (ti == self->index ||
        !ctx->reach[self->index * ctx->n + ti]) {
        err_set(err, "INVALID_REFERENCE", self->id,
                "reference to a non-dependency");
        return 0;
    }
    Step *target = &ctx->steps[ti];
    if (!target->done) {
        err_set(err, "INVALID_REFERENCE", self->id,
                "reference to a step that has not finished");
        return 0;
    }
    if (target->skipped) {
        err_set(err, "SKIPPED_REFERENCE", self->id,
                "reference to a skipped step");
        return 0;
    }
    Value *cur = target->result ? target->result : v_null();

    if (dot < tlen) {
        size_t pos = dot + 1;
        for (;;) {
            size_t next = tlen;
            for (size_t i = pos; i < tlen; i++) {
                if (t[i] == '.') {
                    next = i;
                    break;
                }
            }
            size_t seglen = next - pos;
            const char *seg = t + pos;
            if (seglen == 0) {
                err_set(err, "INVALID_REFERENCE", self->id,
                        "empty path segment");
                return 0;
            }
            if (cur->t == V_OBJ) {
                Value *nv = obj_get_n(cur, seg, seglen);
                if (!nv) {
                    err_set(err, "INVALID_REFERENCE", self->id,
                            "reference path key not found");
                    return 0;
                }
                cur = nv;
            } else if (cur->t == V_ARR) {
                unsigned long long idx = 0;
                int bad = 0;
                for (size_t i = 0; i < seglen; i++) {
                    if (seg[i] < '0' || seg[i] > '9') {
                        bad = 1;
                        break;
                    }
                    unsigned d = (unsigned)(seg[i] - '0');
                    if (idx > (ULLONG_MAX - d) / 10) {
                        bad = 1;
                        break;
                    }
                    idx = idx * 10 + d;
                }
                if (bad || idx >= cur->arr.len) {
                    err_set(err, "INVALID_REFERENCE", self->id,
                            "reference index out of range");
                    return 0;
                }
                cur = cur->arr.items[(size_t)idx];
            } else {
                err_set(err, "INVALID_REFERENCE", self->id,
                        "reference path traverses a scalar");
                return 0;
            }
            if (next == tlen)
                break;
            pos = next + 1;
        }
    }
    *out = cur;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Operation helpers                                                   */
/* ------------------------------------------------------------------ */

static int check_args(const Value *args, const char *const *names, size_t nn,
                      const char *step, Err *err)
{
    for (size_t i = 0; i < args->obj.len; i++) {
        int ok = 0;
        for (size_t j = 0; j < nn; j++) {
            if (key_eq(args->obj.items[i].key, args->obj.items[i].klen,
                       names[j])) {
                ok = 1;
                break;
            }
        }
        if (!ok) {
            err_set(err, "INVALID_ARG", step, "unknown argument name");
            return 0;
        }
    }
    return 1;
}

static int req_int(const Value *args, const char *name, int64_t *out,
                   const char *step, Err *err)
{
    Value *v = obj_get_c(args, name);
    if (!v) {
        err_set(err, "INVALID_ARG", step, "missing required integer arg");
        return 0;
    }
    if (v->t != V_NUM || !v->num.is_int) {
        err_set(err, "INVALID_ARG", step,
                "argument must be an integer literal");
        return 0;
    }
    *out = v->num.i;
    return 1;
}

static int opt_int(const Value *args, const char *name, int64_t *out,
                   int *present, const char *step, Err *err)
{
    Value *v = obj_get_c(args, name);
    if (!v) {
        *present = 0;
        return 1;
    }
    if (v->t != V_NUM || !v->num.is_int) {
        err_set(err, "INVALID_ARG", step,
                "argument must be an integer literal");
        return 0;
    }
    *out = v->num.i;
    *present = 1;
    return 1;
}

static Value *req_str(const Value *args, const char *name, const char *step,
                      Err *err)
{
    Value *v = obj_get_c(args, name);
    if (!v) {
        err_set(err, "INVALID_ARG", step, "missing required string arg");
        return NULL;
    }
    if (v->t != V_STR) {
        err_set(err, "INVALID_ARG", step, "argument must be a string");
        return NULL;
    }
    return v;
}

static Value *req_arr(const Value *args, const char *name, const char *step,
                      Err *err)
{
    Value *v = obj_get_c(args, name);
    if (!v) {
        err_set(err, "INVALID_ARG", step, "missing required array arg");
        return NULL;
    }
    if (v->t != V_ARR) {
        err_set(err, "INVALID_ARG", step, "argument must be an array");
        return NULL;
    }
    return v;
}

static Value *req_obj(const Value *args, const char *name, const char *step,
                      Err *err)
{
    Value *v = obj_get_c(args, name);
    if (!v) {
        err_set(err, "INVALID_ARG", step, "missing required object arg");
        return NULL;
    }
    if (v->t != V_OBJ) {
        err_set(err, "INVALID_ARG", step, "argument must be an object");
        return NULL;
    }
    return v;
}

static Value *req_any(const Value *args, const char *name, const char *step,
                      Err *err)
{
    Value *v = obj_get_c(args, name);
    if (!v) {
        err_set(err, "INVALID_ARG", step, "missing required argument");
        return NULL;
    }
    return v;
}

static Value *req_bool(const Value *args, const char *name, const char *step,
                       Err *err)
{
    Value *v = obj_get_c(args, name);
    if (!v) {
        err_set(err, "INVALID_ARG", step, "missing required boolean arg");
        return NULL;
    }
    if (v->t != V_BOOL) {
        err_set(err, "INVALID_ARG", step, "argument must be a boolean");
        return NULL;
    }
    return v;
}

static Value *req_i64_val(Value *v, const char *step, Err *err)
{
    if (!v || v->t != V_NUM || !v->num.is_int) {
        err_set(err, "INVALID_ARG", step,
                "value must be an integer number");
        return NULL;
    }
    return v;
}

enum { ARITH_OK = 0, ARITH_DIVZERO = 1, ARITH_OVERFLOW = 2 };

static int binop(const char *op, int64_t a, int64_t b, int64_t *out)
{
    if (strcmp(op, "add") == 0)
        return __builtin_add_overflow(a, b, out) ? ARITH_OVERFLOW : ARITH_OK;
    if (strcmp(op, "subtract") == 0)
        return __builtin_sub_overflow(a, b, out) ? ARITH_OVERFLOW : ARITH_OK;
    if (strcmp(op, "multiply") == 0)
        return __builtin_mul_overflow(a, b, out) ? ARITH_OVERFLOW : ARITH_OK;
    if (strcmp(op, "divide") == 0) {
        if (b == 0)
            return ARITH_DIVZERO;
        if (a == INT64_MIN && b == -1)
            return ARITH_OVERFLOW;
        *out = a / b;
        return ARITH_OK;
    }
    if (strcmp(op, "mod") == 0) {
        if (b == 0)
            return ARITH_DIVZERO;
        if (a == INT64_MIN && b == -1)
            return ARITH_OVERFLOW;
        *out = a % b;
        return ARITH_OK;
    }
    return -1;
}

static int arith_fail(int rc, const char *step, Err *err)
{
    if (rc == ARITH_DIVZERO) {
        err_set(err, "DIVISION_BY_ZERO", step, "division by zero");
        return 0;
    }
    err_set(err, "ARITHMETIC_OVERFLOW", step, "i64 arithmetic overflow");
    return 0;
}

/* ------------------------------------------------------------------ */
/* String helpers                                                      */
/* ------------------------------------------------------------------ */

static int is_ascii_ws(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

static size_t utf8_cp_len(const char *s, size_t remaining)
{
    unsigned char c = (unsigned char)s[0];
    size_t n = 1;
    if ((c & 0x80) == 0)
        n = 1;
    else if ((c & 0xE0) == 0xC0)
        n = 2;
    else if ((c & 0xF0) == 0xE0)
        n = 3;
    else if ((c & 0xF8) == 0xF0)
        n = 4;
    if (n > remaining)
        n = remaining;
    if (n == 0)
        n = 1;
    return n;
}

static int contains_bytes(const char *hay, size_t hlen, const char *needle,
                          size_t nlen)
{
    if (nlen == 0)
        return 1;
    if (nlen > hlen)
        return 0;
    for (size_t i = 0; i + nlen <= hlen; i++)
        if (memcmp(hay + i, needle, nlen) == 0)
            return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Total-order stable sort                                             */
/* ------------------------------------------------------------------ */

static void merge_sort_rec(Value **a, Value **tmp, size_t lo, size_t hi)
{
    if (hi - lo < 2)
        return;
    size_t mid = lo + (hi - lo) / 2;
    merge_sort_rec(a, tmp, lo, mid);
    merge_sort_rec(a, tmp, mid, hi);
    size_t i = lo, j = mid, k = lo;
    while (i < mid && j < hi) {
        if (total_cmp(a[i], a[j]) <= 0)
            tmp[k++] = a[i++];
        else
            tmp[k++] = a[j++];
    }
    while (i < mid)
        tmp[k++] = a[i++];
    while (j < hi)
        tmp[k++] = a[j++];
    for (size_t x = lo; x < hi; x++)
        a[x] = tmp[x];
}

static void stable_sort_values(Value **a, size_t n)
{
    if (n < 2)
        return;
    Value **tmp = malloc(n * sizeof(Value *));
    if (!tmp) {
        fprintf(stderr, "workflow: out of memory\n");
        exit(3);
    }
    merge_sort_rec(a, tmp, 0, n);
    free(tmp);
}

/* ------------------------------------------------------------------ */
/* map transforms / filter predicates                                  */
/* ------------------------------------------------------------------ */

static Value *subst_self(const Value *v, const Value *elem)
{
    if (v->t == V_OBJ) {
        if (v->obj.len == 1 &&
            key_eq(v->obj.items[0].key, v->obj.items[0].klen, "$self") &&
            v->obj.items[0].val && v->obj.items[0].val->t == V_BOOL &&
            v->obj.items[0].val->b)
            return (Value *)elem;
        Value *o = v_obj();
        for (size_t i = 0; i < v->obj.len; i++)
            obj_append_n(o, v->obj.items[i].key, v->obj.items[i].klen,
                         subst_self(v->obj.items[i].val, elem));
        return o;
    }
    if (v->t == V_ARR) {
        Value *a = v_arr();
        for (size_t i = 0; i < v->arr.len; i++)
            arr_push(a, subst_self(v->arr.items[i], elem));
        return a;
    }
    return (Value *)v;
}

static int elem_int(const Value *elem, int64_t *out, const char *step,
                    Err *err)
{
    if (elem->t != V_NUM || !elem->num.is_int) {
        err_set(err, "INVALID_ARG", step,
                "array element must be an integer number");
        return 0;
    }
    *out = elem->num.i;
    return 1;
}

static int apply_transform(const Value *elem, const Value *tv, Value **out,
                           const char *step, Err *err)
{
    if (!tv || tv->t != V_OBJ) {
        err_set(err, "INVALID_ARG", step, "transform must be an object");
        return 0;
    }
    Value *typev = obj_get_c(tv, "type");
    if (!typev || typev->t != V_STR) {
        err_set(err, "INVALID_ARG", step,
                "transform.type must be a string");
        return 0;
    }
    const char *type = typev->str.s;

    static const char *opkeys[] = {"type", "op", "args"};
    static const char *valkeys[] = {"type", "value"};
    static const char *onlytype[] = {"type"};

    int64_t ev;
    if (strcmp(type, "op") == 0) {
        if (!check_args(tv, opkeys, 3, step, err))
            return 0;
        Value *opv = obj_get_c(tv, "op");
        if (!opv || opv->t != V_STR) {
            err_set(err, "INVALID_ARG", step,
                    "transform.op must be a string");
            return 0;
        }
        const char *op = opv->str.s;
        if (strcmp(op, "add") != 0 && strcmp(op, "subtract") != 0 &&
            strcmp(op, "multiply") != 0 && strcmp(op, "divide") != 0 &&
            strcmp(op, "mod") != 0) {
            err_set(err, "INVALID_ARG", step,
                    "unsupported transform operation");
            return 0;
        }
        Value *targs = obj_get_c(tv, "args");
        if (!targs)
            targs = v_obj();
        else if (targs->t != V_OBJ) {
            err_set(err, "INVALID_ARG", step,
                    "transform.args must be an object");
            return 0;
        }
        Value *sub = subst_self(targs, elem);
        static const char *abargs[] = {"a", "b"};
        if (!check_args(sub, abargs, 2, step, err))
            return 0;
        int64_t a, b;
        if (!req_int(sub, "a", &a, step, err))
            return 0;
        if (!req_int(sub, "b", &b, step, err))
            return 0;
        int64_t r;
        int rc = binop(op, a, b, &r);
        if (rc != ARITH_OK)
            return arith_fail(rc, step, err);
        *out = v_i64(r);
        return 1;
    }
    if (strcmp(type, "constant") == 0) {
        if (!check_args(tv, valkeys, 2, step, err))
            return 0;
        Value *val = req_any(tv, "value", step, err);
        if (!val)
            return 0;
        *out = val;
        return 1;
    }
    if (strcmp(type, "add") == 0 || strcmp(type, "subtract") == 0 ||
        strcmp(type, "multiply") == 0 || strcmp(type, "divide") == 0 ||
        strcmp(type, "mod") == 0) {
        if (!check_args(tv, valkeys, 2, step, err))
            return 0;
        int64_t b;
        if (!req_int(tv, "value", &b, step, err))
            return 0;
        if (!elem_int(elem, &ev, step, err))
            return 0;
        int64_t r;
        int rc = binop(type, ev, b, &r);
        if (rc != ARITH_OK)
            return arith_fail(rc, step, err);
        *out = v_i64(r);
        return 1;
    }
    if (strcmp(type, "negate") == 0) {
        if (!check_args(tv, onlytype, 1, step, err))
            return 0;
        if (!elem_int(elem, &ev, step, err))
            return 0;
        if (ev == INT64_MIN) {
            err_set(err, "ARITHMETIC_OVERFLOW", step, "negate overflow");
            return 0;
        }
        *out = v_i64(-ev);
        return 1;
    }
    if (strcmp(type, "abs") == 0) {
        if (!check_args(tv, onlytype, 1, step, err))
            return 0;
        if (!elem_int(elem, &ev, step, err))
            return 0;
        if (ev == INT64_MIN) {
            err_set(err, "ARITHMETIC_OVERFLOW", step, "abs overflow");
            return 0;
        }
        *out = v_i64(ev < 0 ? -ev : ev);
        return 1;
    }
    if (strcmp(type, "stringify") == 0) {
        if (!check_args(tv, onlytype, 1, step, err))
            return 0;
        Buf b = {0};
        ser_value(&b, elem);
        Value *s = v_str_n(b.p ? b.p : "", b.len);
        free(b.p);
        *out = s;
        return 1;
    }
    if (strcmp(type, "type") == 0) {
        if (!check_args(tv, onlytype, 1, step, err))
            return 0;
        *out = v_str_c(type_name(elem));
        return 1;
    }
    err_set(err, "INVALID_ARG", step, "unknown transform type");
    return 0;
}

/* Detect the documented duplicate-key form {"type":"type_is","type":"x"}.
   Returns 1 when any "type" pair has the value "type_is". */
static int type_is_shape(const Value *pv)
{
    for (size_t i = 0; i < pv->obj.len; i++) {
        if (key_eq(pv->obj.items[i].key, pv->obj.items[i].klen, "type") &&
            pv->obj.items[i].val && pv->obj.items[i].val->t == V_STR &&
            strcmp(pv->obj.items[i].val->str.s, "type_is") == 0)
            return 1;
    }
    return 0;
}

/* Return the last "type" value that is not the "type_is" discriminator. */
static Value *type_is_duplicate_target(const Value *pv)
{
    Value *target = NULL;
    for (size_t i = 0; i < pv->obj.len; i++) {
        if (!key_eq(pv->obj.items[i].key, pv->obj.items[i].klen, "type"))
            continue;
        Value *v = pv->obj.items[i].val;
        if (v && v->t == V_STR && strcmp(v->str.s, "type_is") == 0)
            continue;
        target = v;
    }
    return target;
}

static int is_type_name_str(const char *s)
{
    return strcmp(s, "null") == 0 || strcmp(s, "boolean") == 0 ||
           strcmp(s, "number") == 0 || strcmp(s, "string") == 0 ||
           strcmp(s, "array") == 0 || strcmp(s, "object") == 0;
}

static int apply_predicate(const Value *elem, const Value *pv, int *keep,
                           const char *step, Err *err)
{
    if (!pv || pv->t != V_OBJ) {
        err_set(err, "INVALID_ARG", step, "predicate must be an object");
        return 0;
    }
    Value *typev = obj_get_c(pv, "type");
    if (!typev || typev->t != V_STR) {
        err_set(err, "INVALID_ARG", step,
                "predicate.type must be a string");
        return 0;
    }
    const char *type = typev->str.s;

    static const char *onlytype[] = {"type"};
    static const char *valkeys[] = {"type", "value"};

    /* The SPEC documents the type_is predicate as
       {"type": "type_is", "type": "<type>"} (a duplicate key), so accept
       the target type from a second "type" pair as well as from the more
       conventional "value"/"target"/"type_name" keys. */
    if (strcmp(type, "type_is") == 0 || type_is_shape(pv) ||
        is_type_name_str(type)) {
        Value *target = obj_get_c(pv, "value");
        if (!target)
            target = obj_get_c(pv, "target");
        if (!target)
            target = obj_get_c(pv, "type_name");
        if (!target)
            target = type_is_duplicate_target(pv);
        if (!target || target->t != V_STR) {
            err_set(err, "INVALID_ARG", step,
                    "type_is requires a string type name");
            return 0;
        }
        const char *wanted = target->str.s;
        static const char *valid[] = {"null", "boolean", "number", "string",
                                      "array", "object"};
        int oktype = 0;
        for (size_t i = 0; i < 6; i++)
            if (strcmp(wanted, valid[i]) == 0)
                oktype = 1;
        if (!oktype) {
            err_set(err, "INVALID_ARG", step, "invalid type name");
            return 0;
        }
        /* reject unknown keys */
        for (size_t i = 0; i < pv->obj.len; i++) {
            Pair *pr = &pv->obj.items[i];
            if (key_eq(pr->key, pr->klen, "type") ||
                key_eq(pr->key, pr->klen, "value") ||
                key_eq(pr->key, pr->klen, "target") ||
                key_eq(pr->key, pr->klen, "type_name"))
                continue;
            err_set(err, "INVALID_ARG", step, "unknown predicate key");
            return 0;
        }
        *keep = strcmp(type_name(elem), wanted) == 0;
        return 1;
    }

    int64_t ev;
    if (strcmp(type, "even") == 0 || strcmp(type, "odd") == 0) {
        if (!check_args(pv, onlytype, 1, step, err))
            return 0;
        if (!elem_int(elem, &ev, step, err))
            return 0;
        int even = (ev % 2 == 0);
        *keep = (strcmp(type, "even") == 0) ? even : !even;
        return 1;
    }
    if (strcmp(type, "gt") == 0 || strcmp(type, "gte") == 0 ||
        strcmp(type, "lt") == 0 || strcmp(type, "lte") == 0) {
        if (!check_args(pv, valkeys, 2, step, err))
            return 0;
        int64_t val;
        if (!req_int(pv, "value", &val, step, err))
            return 0;
        if (!elem_int(elem, &ev, step, err))
            return 0;
        if (strcmp(type, "gt") == 0)
            *keep = ev > val;
        else if (strcmp(type, "gte") == 0)
            *keep = ev >= val;
        else if (strcmp(type, "lt") == 0)
            *keep = ev < val;
        else
            *keep = ev <= val;
        return 1;
    }
    if (strcmp(type, "eq") == 0 || strcmp(type, "ne") == 0) {
        if (!check_args(pv, valkeys, 2, step, err))
            return 0;
        int64_t val;
        if (!req_int(pv, "value", &val, step, err))
            return 0;
        Value tmp; /* value wrapper on stack, not heap */
        memset(&tmp, 0, sizeof tmp);
        tmp.t = V_NUM;
        tmp.num.is_int = 1;
        tmp.num.i = val;
        tmp.num.d = (double)val;
        int eq = equals_value(elem, &tmp);
        *keep = (strcmp(type, "eq") == 0) ? eq : !eq;
        return 1;
    }
    if (strcmp(type, "truthy") == 0 || strcmp(type, "falsy") == 0) {
        if (!check_args(pv, onlytype, 1, step, err))
            return 0;
        int tr = truthy(elem);
        *keep = (strcmp(type, "truthy") == 0) ? tr : !tr;
        return 1;
    }
    if (strcmp(type, "string_contains") == 0) {
        static const char *valkeys2[] = {"type", "value"};
        if (!check_args(pv, valkeys2, 2, step, err))
            return 0;
        Value *needle = req_str(pv, "value", step, err);
        if (!needle)
            return 0;
        if (elem->t != V_STR) {
            err_set(err, "INVALID_ARG", step,
                    "string_contains requires string elements");
            return 0;
        }
        *keep = contains_bytes(elem->str.s, elem->str.len, needle->str.s,
                               needle->str.len);
        return 1;
    }
    err_set(err, "INVALID_ARG", step, "unknown predicate type");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Operations                                                          */
/* ------------------------------------------------------------------ */

static int op_math(const char *op, const Value *args, Value **out,
                   const char *step, Err *err)
{
    static const char *ab[] = {"a", "b"};
    static const char *inp[] = {"input"};
    static const char *rg[] = {"start", "end"};

    if (strcmp(op, "add") == 0 || strcmp(op, "subtract") == 0 ||
        strcmp(op, "multiply") == 0 || strcmp(op, "divide") == 0 ||
        strcmp(op, "mod") == 0) {
        if (!check_args(args, ab, 2, step, err))
            return 0;
        int64_t a, b, r;
        if (!req_int(args, "a", &a, step, err))
            return 0;
        if (!req_int(args, "b", &b, step, err))
            return 0;
        int rc = binop(op, a, b, &r);
        if (rc != ARITH_OK)
            return arith_fail(rc, step, err);
        *out = v_i64(r);
        return 1;
    }
    if (strcmp(op, "sum") == 0) {
        if (!check_args(args, inp, 1, step, err))
            return 0;
        Value *arr = req_arr(args, "input", step, err);
        if (!arr)
            return 0;
        int64_t acc = 0;
        for (size_t i = 0; i < arr->arr.len; i++) {
            Value *ev = req_i64_val(arr->arr.items[i], step, err);
            if (!ev)
                return 0;
            if (__builtin_add_overflow(acc, ev->num.i, &acc)) {
                err_set(err, "ARITHMETIC_OVERFLOW", step, "sum overflow");
                return 0;
            }
        }
        *out = v_i64(acc);
        return 1;
    }
    if (strcmp(op, "min") == 0 || strcmp(op, "max") == 0) {
        if (!check_args(args, inp, 1, step, err))
            return 0;
        Value *arr = req_arr(args, "input", step, err);
        if (!arr)
            return 0;
        if (arr->arr.len == 0) {
            err_set(err, "EMPTY_ARRAY", step,
                    "min/max requires a non-empty array");
            return 0;
        }
        int is_min = (strcmp(op, "min") == 0);
        int64_t best = 0;
        for (size_t i = 0; i < arr->arr.len; i++) {
            Value *ev = req_i64_val(arr->arr.items[i], step, err);
            if (!ev)
                return 0;
            if (i == 0 || (is_min ? ev->num.i < best : ev->num.i > best))
                best = ev->num.i;
        }
        *out = v_i64(best);
        return 1;
    }
    if (strcmp(op, "range") == 0) {
        if (!check_args(args, rg, 2, step, err))
            return 0;
        int64_t start, end;
        if (!req_int(args, "start", &start, step, err))
            return 0;
        if (!req_int(args, "end", &end, step, err))
            return 0;
        Value *a = v_arr();
        if (start <= end) {
            int64_t x = start;
            for (;;) {
                arr_push(a, v_i64(x));
                if (x == end)
                    break;
                x++;
            }
        }
        *out = a;
        return 1;
    }
    err_set(err, "INTERNAL_ERROR", step, "unhandled math op");
    return 0;
}

static int op_strings(const char *op, const Value *args, Value **out,
                      const char *step, Err *err)
{
    static const char *inp[] = {"input"};
    static const char *ab[] = {"a", "b"};
    static const char *sd[] = {"input", "sep"};
    static const char *rep[] = {"input", "search", "replace"};

    if (strcmp(op, "concat") == 0) {
        if (!check_args(args, ab, 2, step, err))
            return 0;
        Value *a = req_str(args, "a", step, err);
        Value *b = req_str(args, "b", step, err);
        if (!a || !b)
            return 0;
        Buf buf = {0};
        buf_put(&buf, a->str.s, a->str.len);
        buf_put(&buf, b->str.s, b->str.len);
        *out = v_str_n(buf.p ? buf.p : "", buf.len);
        free(buf.p);
        return 1;
    }
    if (strcmp(op, "split") == 0) {
        if (!check_args(args, sd, 2, step, err))
            return 0;
        Value *in = req_str(args, "input", step, err);
        Value *sep = req_str(args, "sep", step, err);
        if (!in || !sep)
            return 0;
        Value *a = v_arr();
        if (sep->str.len == 0) {
            size_t pos = 0;
            while (pos < in->str.len) {
                size_t cl = utf8_cp_len(in->str.s + pos, in->str.len - pos);
                arr_push(a, v_str_n(in->str.s + pos, cl));
                pos += cl;
            }
        } else {
            size_t pos = 0;
            for (;;) {
                size_t start = pos;
                size_t idx = (size_t)-1;
                for (size_t i = pos; i + sep->str.len <= in->str.len; i++) {
                    if (memcmp(in->str.s + i, sep->str.s, sep->str.len) ==
                        0) {
                        idx = i;
                        break;
                    }
                }
                if (idx == (size_t)-1) {
                    arr_push(a, v_str_n(in->str.s + start,
                                        in->str.len - start));
                    break;
                }
                arr_push(a, v_str_n(in->str.s + start, idx - start));
                pos = idx + sep->str.len;
            }
        }
        *out = a;
        return 1;
    }
    if (strcmp(op, "join") == 0) {
        if (!check_args(args, sd, 2, step, err))
            return 0;
        Value *in = req_arr(args, "input", step, err);
        Value *sep = req_str(args, "sep", step, err);
        if (!in || !sep)
            return 0;
        Buf buf = {0};
        for (size_t i = 0; i < in->arr.len; i++) {
            Value *e = in->arr.items[i];
            if (e->t != V_STR) {
                err_set(err, "INVALID_ARG", step,
                        "join input must be an array of strings");
                free(buf.p);
                return 0;
            }
            if (i)
                buf_put(&buf, sep->str.s, sep->str.len);
            buf_put(&buf, e->str.s, e->str.len);
        }
        *out = v_str_n(buf.p ? buf.p : "", buf.len);
        free(buf.p);
        return 1;
    }
    if (strcmp(op, "uppercase") == 0 || strcmp(op, "lowercase") == 0) {
        if (!check_args(args, inp, 1, step, err))
            return 0;
        Value *in = req_str(args, "input", step, err);
        if (!in)
            return 0;
        Buf buf = {0};
        buf_put(&buf, in->str.s, in->str.len);
        int up = (strcmp(op, "uppercase") == 0);
        for (size_t i = 0; i < buf.len; i++) {
            unsigned char c = (unsigned char)buf.p[i];
            if (up && c >= 'a' && c <= 'z')
                buf.p[i] = (char)(c - 32);
            else if (!up && c >= 'A' && c <= 'Z')
                buf.p[i] = (char)(c + 32);
        }
        *out = v_str_n(buf.p ? buf.p : "", buf.len);
        free(buf.p);
        return 1;
    }
    if (strcmp(op, "trim") == 0) {
        if (!check_args(args, inp, 1, step, err))
            return 0;
        Value *in = req_str(args, "input", step, err);
        if (!in)
            return 0;
        size_t start = 0, end = in->str.len;
        while (start < end && is_ascii_ws((unsigned char)in->str.s[start]))
            start++;
        while (end > start && is_ascii_ws((unsigned char)in->str.s[end - 1]))
            end--;
        *out = v_str_n(in->str.s + start, end - start);
        return 1;
    }
    if (strcmp(op, "replace") == 0) {
        if (!check_args(args, rep, 3, step, err))
            return 0;
        Value *in = req_str(args, "input", step, err);
        Value *search = req_str(args, "search", step, err);
        Value *repl = req_str(args, "replace", step, err);
        if (!in || !search || !repl)
            return 0;
        if (search->str.len == 0) {
            err_set(err, "INVALID_ARG", step,
                    "replace search must be non-empty");
            return 0;
        }
        Buf buf = {0};
        size_t i = 0;
        while (i < in->str.len) {
            if (i + search->str.len <= in->str.len &&
                memcmp(in->str.s + i, search->str.s, search->str.len) == 0) {
                buf_put(&buf, repl->str.s, repl->str.len);
                i += search->str.len;
            } else {
                buf_putc(&buf, in->str.s[i]);
                i++;
            }
        }
        *out = v_str_n(buf.p ? buf.p : "", buf.len);
        free(buf.p);
        return 1;
    }
    if (strcmp(op, "length") == 0) {
        if (!check_args(args, inp, 1, step, err))
            return 0;
        Value *in = req_str(args, "input", step, err);
        if (!in)
            return 0;
        int64_t count = 0;
        for (size_t i = 0; i < in->str.len; i++) {
            unsigned char c = (unsigned char)in->str.s[i];
            if ((c & 0xC0) != 0x80)
                count++;
        }
        *out = v_i64(count);
        return 1;
    }
    err_set(err, "INTERNAL_ERROR", step, "unhandled string op");
    return 0;
}

static int op_arrays(const char *op, const Value *args, Value **out,
                     const char *step, Err *err)
{
    static const char *inp[] = {"input"};
    static const char *mp[] = {"input", "transform"};
    static const char *fl[] = {"input", "predicate"};
    static const char *red[] = {"input", "op", "initial"};
    static const char *sl[] = {"input", "start", "end"};

    if (strcmp(op, "map") == 0) {
        if (!check_args(args, mp, 2, step, err))
            return 0;
        Value *in = req_arr(args, "input", step, err);
        Value *tv = req_any(args, "transform", step, err);
        if (!in || !tv)
            return 0;
        if (tv->t == V_STR) {
            Value *o = v_obj();
            obj_put_c(o, "type", tv);
            tv = o;
        }
        Value *a = v_arr();
        for (size_t i = 0; i < in->arr.len; i++) {
            Value *r;
            if (!apply_transform(in->arr.items[i], tv, &r, step, err))
                return 0;
            arr_push(a, r);
        }
        *out = a;
        return 1;
    }
    if (strcmp(op, "filter") == 0) {
        if (!check_args(args, fl, 2, step, err))
            return 0;
        Value *in = req_arr(args, "input", step, err);
        Value *pv = req_any(args, "predicate", step, err);
        if (!in || !pv)
            return 0;
        /* PRD examples use a bare string shorthand like "even". */
        if (pv->t == V_STR) {
            Value *o = v_obj();
            obj_put_c(o, "type", pv);
            pv = o;
        }
        Value *a = v_arr();
        for (size_t i = 0; i < in->arr.len; i++) {
            int keep;
            if (!apply_predicate(in->arr.items[i], pv, &keep, step, err))
                return 0;
            if (keep)
                arr_push(a, in->arr.items[i]);
        }
        *out = a;
        return 1;
    }
    if (strcmp(op, "reduce") == 0) {
        if (!check_args(args, red, 3, step, err))
            return 0;
        Value *in = req_arr(args, "input", step, err);
        Value *opv = req_str(args, "op", step, err);
        if (!in || !opv)
            return 0;
        const char *rop = opv->str.s;
        if (strcmp(rop, "add") != 0 && strcmp(rop, "subtract") != 0 &&
            strcmp(rop, "multiply") != 0) {
            err_set(err, "INVALID_ARG", step, "unsupported reduce operation");
            return 0;
        }
        int64_t acc;
        int present;
        if (!opt_int(args, "initial", &acc, &present, step, err))
            return 0;
        if (!present)
            acc = (strcmp(rop, "multiply") == 0) ? 1 : 0;
        for (size_t i = 0; i < in->arr.len; i++) {
            Value *ev = req_i64_val(in->arr.items[i], step, err);
            if (!ev)
                return 0;
            int64_t r;
            int rc = binop(rop, acc, ev->num.i, &r);
            if (rc != ARITH_OK)
                return arith_fail(rc, step, err);
            acc = r;
        }
        *out = v_i64(acc);
        return 1;
    }
    if (strcmp(op, "sort") == 0) {
        if (!check_args(args, inp, 1, step, err))
            return 0;
        Value *in = req_arr(args, "input", step, err);
        if (!in)
            return 0;
        Value *a = v_arr();
        for (size_t i = 0; i < in->arr.len; i++)
            arr_push(a, in->arr.items[i]);
        stable_sort_values(a->arr.items, a->arr.len);
        *out = a;
        return 1;
    }
    if (strcmp(op, "reverse") == 0) {
        if (!check_args(args, inp, 1, step, err))
            return 0;
        Value *in = req_arr(args, "input", step, err);
        if (!in)
            return 0;
        Value *a = v_arr();
        for (size_t i = in->arr.len; i > 0; i--)
            arr_push(a, in->arr.items[i - 1]);
        *out = a;
        return 1;
    }
    if (strcmp(op, "unique") == 0) {
        if (!check_args(args, inp, 1, step, err))
            return 0;
        Value *in = req_arr(args, "input", step, err);
        if (!in)
            return 0;
        Value *a = v_arr();
        size_t cap = 16;
        while (cap < in->arr.len * 2 + 1) {
            if (cap > ((size_t)-1) / 2)
                break;
            cap *= 2;
        }
        long *slots = malloc(cap * sizeof(long));
        if (!slots) {
            fprintf(stderr, "workflow: out of memory\n");
            exit(3);
        }
        for (size_t i = 0; i < cap; i++)
            slots[i] = -1;
        for (size_t i = 0; i < in->arr.len; i++) {
            Value *e = in->arr.items[i];
            uint64_t h = hash_value(e);
            size_t pos = (size_t)(h & (cap - 1));
            int seen = 0;
            while (slots[pos] != -1) {
                if (equals_value(e, a->arr.items[(size_t)slots[pos]])) {
                    seen = 1;
                    break;
                }
                pos = (pos + 1) & (cap - 1);
            }
            if (!seen) {
                slots[pos] = (long)a->arr.len;
                arr_push(a, e);
            }
        }
        free(slots);
        *out = a;
        return 1;
    }
    if (strcmp(op, "slice") == 0) {
        if (!check_args(args, sl, 3, step, err))
            return 0;
        Value *in = req_arr(args, "input", step, err);
        int64_t start, end;
        int have_end;
        if (!in)
            return 0;
        if (!req_int(args, "start", &start, step, err))
            return 0;
        if (!opt_int(args, "end", &end, &have_end, step, err))
            return 0;
        int64_t n = (int64_t)in->arr.len;
        if (!have_end)
            end = n;
        if (start < 0)
            start += n;
        if (end < 0)
            end += n;
        if (start < 0)
            start = 0;
        if (end < 0)
            end = 0;
        if (start > n)
            start = n;
        if (end > n)
            end = n;
        Value *a = v_arr();
        if (start < end)
            for (int64_t i = start; i < end; i++)
                arr_push(a, in->arr.items[(size_t)i]);
        *out = a;
        return 1;
    }
    if (strcmp(op, "flatten") == 0) {
        if (!check_args(args, inp, 1, step, err))
            return 0;
        Value *in = req_arr(args, "input", step, err);
        if (!in)
            return 0;
        Value *a = v_arr();
        for (size_t i = 0; i < in->arr.len; i++) {
            Value *e = in->arr.items[i];
            if (e->t == V_ARR) {
                for (size_t j = 0; j < e->arr.len; j++)
                    arr_push(a, e->arr.items[j]);
            } else {
                arr_push(a, e);
            }
        }
        *out = a;
        return 1;
    }
    err_set(err, "INTERNAL_ERROR", step, "unhandled array op");
    return 0;
}

static int key_in_array(const Value *keys, const char *k, size_t klen)
{
    for (size_t i = 0; i < keys->arr.len; i++) {
        Value *e = keys->arr.items[i];
        if (e->t == V_STR && e->str.len == klen &&
            memcmp(e->str.s, k, klen) == 0)
            return 1;
    }
    return 0;
}

static int op_objects(const char *op, const Value *args, Value **out,
                      const char *step, Err *err)
{
    static const char *inp[] = {"input"};
    static const char *gk[] = {"input", "key"};
    static const char *sk[] = {"input", "key", "value"};
    static const char *ab[] = {"a", "b"};
    static const char *pk[] = {"input", "keys"};

    if (strcmp(op, "get") == 0) {
        if (!check_args(args, gk, 2, step, err))
            return 0;
        Value *in = req_obj(args, "input", step, err);
        Value *key = req_str(args, "key", step, err);
        if (!in || !key)
            return 0;
        Value *v = obj_get_n(in, key->str.s, key->str.len);
        *out = v ? v : v_null();
        return 1;
    }
    if (strcmp(op, "set") == 0) {
        if (!check_args(args, sk, 3, step, err))
            return 0;
        Value *in = req_obj(args, "input", step, err);
        Value *key = req_str(args, "key", step, err);
        Value *val = req_any(args, "value", step, err);
        if (!in || !key || !val)
            return 0;
        Value *o = v_obj();
        int found = 0;
        for (size_t i = 0; i < in->obj.len; i++) {
            if (in->obj.items[i].klen == key->str.len &&
                memcmp(in->obj.items[i].key, key->str.s, key->str.len) ==
                    0) {
                obj_put_n(o, in->obj.items[i].key, in->obj.items[i].klen,
                          val);
                found = 1;
            } else {
                obj_put_n(o, in->obj.items[i].key, in->obj.items[i].klen,
                          in->obj.items[i].val);
            }
        }
        if (!found)
            obj_put_n(o, key->str.s, key->str.len, val);
        *out = o;
        return 1;
    }
    if (strcmp(op, "keys") == 0) {
        if (!check_args(args, inp, 1, step, err))
            return 0;
        Value *in = req_obj(args, "input", step, err);
        if (!in)
            return 0;
        size_t n;
        Pair **sp = sorted_pairs(in, &n);
        Value *a = v_arr();
        for (size_t i = 0; i < n; i++)
            arr_push(a, v_str_n(sp[i]->key, sp[i]->klen));
        free(sp);
        *out = a;
        return 1;
    }
    if (strcmp(op, "values") == 0) {
        if (!check_args(args, inp, 1, step, err))
            return 0;
        Value *in = req_obj(args, "input", step, err);
        if (!in)
            return 0;
        size_t n;
        Pair **sp = sorted_pairs(in, &n);
        Value *a = v_arr();
        for (size_t i = 0; i < n; i++)
            arr_push(a, sp[i]->val);
        free(sp);
        *out = a;
        return 1;
    }
    if (strcmp(op, "merge") == 0) {
        if (!check_args(args, ab, 2, step, err))
            return 0;
        Value *a1 = req_obj(args, "a", step, err);
        Value *b1 = req_obj(args, "b", step, err);
        if (!a1 || !b1)
            return 0;
        Value *o = v_obj();
        for (size_t i = 0; i < a1->obj.len; i++)
            obj_put_n(o, a1->obj.items[i].key, a1->obj.items[i].klen,
                      a1->obj.items[i].val);
        for (size_t i = 0; i < b1->obj.len; i++)
            obj_put_n(o, b1->obj.items[i].key, b1->obj.items[i].klen,
                      b1->obj.items[i].val);
        *out = o;
        return 1;
    }
    if (strcmp(op, "pick") == 0) {
        if (!check_args(args, pk, 2, step, err))
            return 0;
        Value *in = req_obj(args, "input", step, err);
        Value *keys = req_arr(args, "keys", step, err);
        if (!in || !keys)
            return 0;
        for (size_t i = 0; i < keys->arr.len; i++) {
            if (keys->arr.items[i]->t != V_STR) {
                err_set(err, "INVALID_ARG", step,
                        "pick keys must be strings");
                return 0;
            }
        }
        /* dedupe + sort requested keys */
        size_t nk = keys->arr.len;
        Pair *kp = NULL;
        if (nk) {
            kp = calloc(nk, sizeof(Pair));
            if (!kp) {
                fprintf(stderr, "workflow: out of memory\n");
                exit(3);
            }
            size_t used = 0;
            for (size_t i = 0; i < nk; i++) {
                Value *k = keys->arr.items[i];
                int dup = 0;
                for (size_t j = 0; j < used; j++) {
                    if (kp[j].klen == k->str.len &&
                        memcmp(kp[j].key, k->str.s, k->str.len) == 0) {
                        dup = 1;
                        break;
                    }
                }
                if (!dup) {
                    kp[used].key = k->str.s;
                    kp[used].klen = k->str.len;
                    kp[used].val = NULL;
                    used++;
                }
            }
            nk = used;
            /* insertion sort by key bytes */
            for (size_t i = 1; i < nk; i++) {
                Pair tmp = kp[i];
                size_t j = i;
                while (j > 0 &&
                       cmp_bytes(kp[j - 1].key, kp[j - 1].klen, tmp.key,
                                 tmp.klen) > 0) {
                    kp[j] = kp[j - 1];
                    j--;
                }
                kp[j] = tmp;
            }
        }
        Value *o = v_obj();
        for (size_t i = 0; i < nk; i++) {
            Value *v = obj_get_n(in, kp[i].key, kp[i].klen);
            obj_put_n(o, kp[i].key, kp[i].klen, v ? v : v_null());
        }
        free(kp);
        *out = o;
        return 1;
    }
    if (strcmp(op, "omit") == 0) {
        if (!check_args(args, pk, 2, step, err))
            return 0;
        Value *in = req_obj(args, "input", step, err);
        Value *keys = req_arr(args, "keys", step, err);
        if (!in || !keys)
            return 0;
        for (size_t i = 0; i < keys->arr.len; i++) {
            if (keys->arr.items[i]->t != V_STR) {
                err_set(err, "INVALID_ARG", step,
                        "omit keys must be strings");
                return 0;
            }
        }
        Value *o = v_obj();
        for (size_t i = 0; i < in->obj.len; i++) {
            if (!key_in_array(keys, in->obj.items[i].key,
                              in->obj.items[i].klen))
                obj_put_n(o, in->obj.items[i].key, in->obj.items[i].klen,
                          in->obj.items[i].val);
        }
        *out = o;
        return 1;
    }
    err_set(err, "INTERNAL_ERROR", step, "unhandled object op");
    return 0;
}

static int op_utils(const char *op, const Value *args, Step *s, Value **out,
                    Err *err)
{
    const char *step = s->id;
    static const char *ab[] = {"a", "b"};
    static const char *cmpk[] = {"a", "b", "op"};
    static const char *one[] = {"a"};
    static const char *val[] = {"value"};
    static const char *fu[] = {"failures", "value"};

    if (strcmp(op, "constant") == 0) {
        if (!check_args(args, val, 1, step, err))
            return 0;
        Value *v = req_any(args, "value", step, err);
        if (!v)
            return 0;
        *out = v;
        return 1;
    }
    if (strcmp(op, "equals") == 0) {
        if (!check_args(args, ab, 2, step, err))
            return 0;
        Value *a = req_any(args, "a", step, err);
        Value *b = req_any(args, "b", step, err);
        if (!a || !b)
            return 0;
        *out = v_bool(equals_value(a, b));
        return 1;
    }
    if (strcmp(op, "compare") == 0) {
        if (!check_args(args, cmpk, 3, step, err))
            return 0;
        Value *a = req_any(args, "a", step, err);
        Value *b = req_any(args, "b", step, err);
        Value *opv = req_str(args, "op", step, err);
        if (!a || !b || !opv)
            return 0;
        const char *c = opv->str.s;
        int cmp = total_cmp(a, b);
        int r;
        if (strcmp(c, "gt") == 0)
            r = cmp > 0;
        else if (strcmp(c, "gte") == 0)
            r = cmp >= 0;
        else if (strcmp(c, "lt") == 0)
            r = cmp < 0;
        else if (strcmp(c, "lte") == 0)
            r = cmp <= 0;
        else {
            err_set(err, "INVALID_ARG", step, "invalid compare operator");
            return 0;
        }
        *out = v_bool(r);
        return 1;
    }
    if (strcmp(op, "and") == 0 || strcmp(op, "or") == 0) {
        if (!check_args(args, ab, 2, step, err))
            return 0;
        Value *a = req_bool(args, "a", step, err);
        Value *b = req_bool(args, "b", step, err);
        if (!a || !b)
            return 0;
        if (strcmp(op, "and") == 0)
            *out = v_bool(a->b && b->b);
        else
            *out = v_bool(a->b || b->b);
        return 1;
    }
    if (strcmp(op, "not") == 0) {
        if (!check_args(args, one, 1, step, err))
            return 0;
        Value *a = req_bool(args, "a", step, err);
        if (!a)
            return 0;
        *out = v_bool(!a->b);
        return 1;
    }
    if (strcmp(op, "type") == 0) {
        if (!check_args(args, val, 1, step, err))
            return 0;
        Value *v = req_any(args, "value", step, err);
        if (!v)
            return 0;
        *out = v_str_c(type_name(v));
        return 1;
    }
    if (strcmp(op, "fail_until") == 0) {
        if (!check_args(args, fu, 2, step, err))
            return 0;
        int64_t failures;
        if (!req_int(args, "failures", &failures, step, err))
            return 0;
        if (failures < 0 || failures > 10) {
            err_set(err, "INVALID_ARG", step,
                    "fail_until failures must be in 0..10");
            return 0;
        }
        Value *v = req_any(args, "value", step, err);
        if (!v)
            return 0;
        s->fail_attempts++;
        if (s->fail_attempts <= failures) {
            err_set(err, "TRANSIENT_FAILURE", step,
                    "transient failure from fail_until");
            return 0;
        }
        *out = v;
        return 1;
    }
    err_set(err, "INTERNAL_ERROR", step, "unhandled utility op");
    return 0;
}

/* Returns 1 on success, 0 on failure. */
static int run_op(Step *s, const Value *args, Value **out, Err *err)
{
    const char *op = s->op;
    const char *step = s->id;

    if (strcmp(op, "add") == 0 || strcmp(op, "subtract") == 0 ||
        strcmp(op, "multiply") == 0 || strcmp(op, "divide") == 0 ||
        strcmp(op, "mod") == 0 || strcmp(op, "sum") == 0 ||
        strcmp(op, "min") == 0 || strcmp(op, "max") == 0 ||
        strcmp(op, "range") == 0)
        return op_math(op, args, out, step, err);

    if (strcmp(op, "concat") == 0 || strcmp(op, "split") == 0 ||
        strcmp(op, "join") == 0 || strcmp(op, "uppercase") == 0 ||
        strcmp(op, "lowercase") == 0 || strcmp(op, "trim") == 0 ||
        strcmp(op, "replace") == 0 || strcmp(op, "length") == 0)
        return op_strings(op, args, out, step, err);

    if (strcmp(op, "map") == 0 || strcmp(op, "filter") == 0 ||
        strcmp(op, "reduce") == 0 || strcmp(op, "sort") == 0 ||
        strcmp(op, "reverse") == 0 || strcmp(op, "unique") == 0 ||
        strcmp(op, "slice") == 0 || strcmp(op, "flatten") == 0)
        return op_arrays(op, args, out, step, err);

    if (strcmp(op, "get") == 0 || strcmp(op, "set") == 0 ||
        strcmp(op, "keys") == 0 || strcmp(op, "values") == 0 ||
        strcmp(op, "merge") == 0 || strcmp(op, "pick") == 0 ||
        strcmp(op, "omit") == 0)
        return op_objects(op, args, out, step, err);

    return op_utils(op, args, s, out, err);
}

/* ------------------------------------------------------------------ */
/* Conditions                                                          */
/* ------------------------------------------------------------------ */

static int eval_condition(const Value *c, Ctx *ctx, Step *self, Value **out,
                          Err *err)
{
    Value *opv = obj_get_c(c, "op");
    if (!opv || opv->t != V_STR) {
        err_set(err, "INVALID_CONDITION", self->id,
                "condition op must be a string");
        return 0;
    }
    const char *op = opv->str.s;

    if (strcmp(op, "eq") == 0 || strcmp(op, "ne") == 0 ||
        strcmp(op, "gt") == 0 || strcmp(op, "gte") == 0 ||
        strcmp(op, "lt") == 0 || strcmp(op, "lte") == 0) {
        Value *lv = obj_get_c(c, "left");
        Value *rv = obj_get_c(c, "right");
        if (!lv || !rv) {
            err_set(err, "INVALID_CONDITION", self->id,
                    "condition is missing an operand");
            return 0;
        }
        Value *l = NULL, *r = NULL;
        if (!resolve_value(lv, ctx, self, &l, err, 0))
            return 0;
        if (!resolve_value(rv, ctx, self, &r, err, 0))
            return 0;
        int cmp = total_cmp(l, r);
        int res;
        if (strcmp(op, "eq") == 0)
            res = equals_value(l, r);
        else if (strcmp(op, "ne") == 0)
            res = !equals_value(l, r);
        else if (strcmp(op, "gt") == 0)
            res = cmp > 0;
        else if (strcmp(op, "gte") == 0)
            res = cmp >= 0;
        else if (strcmp(op, "lt") == 0)
            res = cmp < 0;
        else
            res = cmp <= 0;
        *out = v_bool(res);
        return 1;
    }
    if (strcmp(op, "all") == 0 || strcmp(op, "any") == 0) {
        Value *cs = obj_get_c(c, "conditions");
        if (!cs || cs->t != V_ARR) {
            err_set(err, "INVALID_CONDITION", self->id,
                    "compound condition needs a conditions array");
            return 0;
        }
        int is_all = (strcmp(op, "all") == 0);
        int acc = is_all;
        for (size_t i = 0; i < cs->arr.len; i++) {
            Value *r = NULL;
            if (!eval_condition(cs->arr.items[i], ctx, self, &r, err))
                return 0;
            if (!r || r->t != V_BOOL) {
                err_set(err, "INVALID_CONDITION", self->id,
                        "condition did not yield a boolean");
                return 0;
            }
            if (is_all) {
                if (!r->b) {
                    acc = 0;
                    break;
                }
            } else {
                if (r->b) {
                    acc = 1;
                    break;
                }
            }
        }
        *out = v_bool(acc);
        return 1;
    }
    if (strcmp(op, "not") == 0) {
        Value *inner = obj_get_c(c, "condition");
        if (!inner) {
            err_set(err, "INVALID_CONDITION", self->id,
                    "not condition needs a condition");
            return 0;
        }
        Value *r = NULL;
        if (!eval_condition(inner, ctx, self, &r, err))
            return 0;
        if (!r || r->t != V_BOOL) {
            err_set(err, "INVALID_CONDITION", self->id,
                    "condition did not yield a boolean");
            return 0;
        }
        *out = v_bool(!r->b);
        return 1;
    }
    err_set(err, "INVALID_CONDITION", self->id, "unknown condition operator");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Response construction                                               */
/* ------------------------------------------------------------------ */

static Value *make_error_obj(const Err *err)
{
    Value *e = v_obj();
    obj_put_c(e, "code", v_str_c(err->code ? err->code : "INTERNAL_ERROR"));
    if (err->step)
        obj_put_c(e, "step", v_str_c(err->step));
    else
        obj_put_c(e, "step", v_null());
    obj_put_c(e, "message", v_str_c(err->msg));
    return e;
}

static Value *make_response(const char *status, Step **order, size_t norder,
                            Step **skipped, size_t nskipped, Err *err)
{
    Value *resp = v_obj();
    obj_put_c(resp, "status", v_str_c(status));

    Value *results = v_obj();
    for (size_t i = 0; i < norder; i++) {
        Step *s = order[i];
        Value *v = s->skipped ? v_null() : (s->result ? s->result : v_null());
        obj_put_c(results, s->id, v);
    }
    obj_put_c(resp, "results", results);

    Value *ord = v_arr();
    for (size_t i = 0; i < norder; i++)
        arr_push(ord, v_str_c(order[i]->id));
    obj_put_c(resp, "execution_order", ord);

    Value *sk = v_arr();
    for (size_t i = 0; i < nskipped; i++)
        arr_push(sk, v_str_c(skipped[i]->id));
    obj_put_c(resp, "skipped", sk);

    obj_put_c(resp, "error", err ? make_error_obj(err) : v_null());
    return resp;
}

static int exit_for_code(const char *code)
{
    if (!code)
        return 0;
    if (strcmp(code, "INVALID_WORKFLOW") == 0 ||
        strcmp(code, "UNKNOWN_DEPENDENCY") == 0 ||
        strcmp(code, "CYCLIC_DEPENDENCY") == 0)
        return 2;
    if (strcmp(code, "INTERNAL_ERROR") == 0)
        return 3;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Execution                                                           */
/* ------------------------------------------------------------------ */

static void compute_reach(Ctx *ctx)
{
    size_t n = ctx->n;
    size_t cells = n ? n * n : 1;
    ctx->reach = calloc(cells, 1);
    if (!ctx->reach) {
        fprintf(stderr, "workflow: out of memory\n");
        exit(3);
    }
    /* iterative DFS from each node over depends_on edges */
    size_t *stack = malloc((n ? n : 1) * sizeof(size_t));
    if (!stack) {
        fprintf(stderr, "workflow: out of memory\n");
        exit(3);
    }
    for (size_t i = 0; i < n; i++) {
        size_t sp = 0;
        stack[sp++] = i;
        while (sp > 0) {
            size_t u = stack[--sp];
            Step *su = &ctx->steps[u];
            for (size_t k = 0; k < su->ndeps; k++) {
                size_t v = su->depidx[k];
                if (!ctx->reach[i * n + v]) {
                    ctx->reach[i * n + v] = 1;
                    stack[sp++] = v;
                }
            }
        }
    }
    free(stack);
}

static int pick_ready(Ctx *ctx, size_t *out_idx)
{
    for (size_t i = 0; i < ctx->n; i++) {
        Step *s = &ctx->steps[i];
        if (s->done)
            continue;
        int ready = 1;
        for (size_t k = 0; k < s->ndeps; k++) {
            if (!ctx->steps[s->depidx[k]].done) {
                ready = 0;
                break;
            }
        }
        if (ready) {
            *out_idx = i;
            return 1;
        }
    }
    return 0;
}

static int execute(const Value *doc, Value **out_resp)
{
    Err err;
    memset(&err, 0, sizeof err);
    Step *steps = NULL;
    size_t n = 0;

    if (!validate_document(doc, &steps, &n, &err)) {
        *out_resp = make_response("failed", NULL, 0, NULL, 0, &err);
        return exit_for_code(err.code);
    }

    Ctx ctx;
    ctx.steps = steps;
    ctx.n = n;
    ctx.reach = NULL;
    compute_reach(&ctx);

    Step **order = calloc(n ? n : 1, sizeof(Step *));
    Step **skipped = calloc(n ? n : 1, sizeof(Step *));
    if (!order || !skipped) {
        fprintf(stderr, "workflow: out of memory\n");
        exit(3);
    }
    size_t norder = 0, nskipped = 0, finished = 0;

    while (finished < n) {
        size_t idx;
        if (!pick_ready(&ctx, &idx)) {
            err_set(&err, "INTERNAL_ERROR", NULL,
                    "scheduler could not find a ready step");
            *out_resp = make_response("failed", order, norder, skipped,
                                      nskipped, &err);
            return 3;
        }
        Step *s = &steps[idx];

        /* condition */
        if (s->when) {
            Err cerr;
            memset(&cerr, 0, sizeof cerr);
            Value *cres = NULL;
            if (!eval_condition(s->when, &ctx, s, &cres, &cerr)) {
                *out_resp = make_response("failed", order, norder, skipped,
                                          nskipped, &cerr);
                return exit_for_code(cerr.code);
            }
            if (!cres || cres->t != V_BOOL) {
                err_set(&err, "INVALID_CONDITION", s->id,
                        "condition did not yield a boolean");
                *out_resp = make_response("failed", order, norder, skipped,
                                          nskipped, &err);
                return 1;
            }
            if (!cres->b) {
                s->skipped = 1;
                s->done = 1;
                s->result = NULL;
                order[norder++] = s;
                skipped[nskipped++] = s;
                finished++;
                continue;
            }
        }

        /* resolve args */
        Err rerr;
        memset(&rerr, 0, sizeof rerr);
        Value *rargs = NULL;
        if (!resolve_value(s->args, &ctx, s, &rargs, &rerr, 0)) {
            *out_resp = make_response("failed", order, norder, skipped,
                                      nskipped, &rerr);
            return exit_for_code(rerr.code);
        }

        /* attempts */
        int ok = 0;
        Value *res = NULL;
        Err operr;
        memset(&operr, 0, sizeof operr);
        for (int attempt = 1; attempt <= s->max_attempts; attempt++) {
            Err e;
            memset(&e, 0, sizeof e);
            Value *r = NULL;
            if (run_op(s, rargs, &r, &e)) {
                ok = 1;
                res = r;
                break;
            }
            operr = e;
            if (e.code && strcmp(e.code, "TRANSIENT_FAILURE") == 0 &&
                attempt < s->max_attempts)
                continue;
            break;
        }

        if (!ok) {
            const char *code = operr.code ? operr.code : "INTERNAL_ERROR";
            if (strcmp(code, "TRANSIENT_FAILURE") == 0)
                code = "STEP_FAILED";
            err_set(&err, code, s->id, operr.msg);
            *out_resp = make_response("failed", order, norder, skipped,
                                      nskipped, &err);
            return exit_for_code(code);
        }

        s->result = res;
        s->done = 1;
        order[norder++] = s;
        finished++;
    }

    *out_resp = make_response("completed", order, norder, skipped, nskipped,
                              NULL);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Input reading                                                       */
/* ------------------------------------------------------------------ */

static char *read_all(FILE *f, size_t *out_len)
{
    size_t cap = 65536, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        fprintf(stderr, "workflow: out of memory\n");
        exit(3);
    }
    for (;;) {
        if (len == cap) {
            if (cap > (size_t)-1 / 2) {
                fprintf(stderr, "workflow: input too large\n");
                exit(2);
            }
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) {
                fprintf(stderr, "workflow: out of memory\n");
                exit(3);
            }
            buf = nb;
        }
        size_t r = fread(buf + len, 1, cap - len, f);
        len += r;
        if (r == 0) {
            if (ferror(f)) {
                free(buf);
                return NULL;
            }
            break;
        }
    }
    *out_len = len;
    return buf;
}

static void emit_response(const Value *resp)
{
    Buf b = {0};
    ser_value(&b, resp);
    buf_putc(&b, '\n');
    fwrite(b.p, 1, b.len, stdout);
    fflush(stdout);
    free(b.p);
}

static int emit_simple_failure(const char *code, const char *message)
{
    Err err;
    memset(&err, 0, sizeof err);
    err_set(&err, code, NULL, message);
    Value *resp = make_response("failed", NULL, 0, NULL, 0, &err);
    emit_response(resp);
    return exit_for_code(code);
}

int main(int argc, char **argv)
{
    if (argc != 3 || strcmp(argv[1], "run") != 0)
        return emit_simple_failure("INVALID_WORKFLOW",
                                   "usage: workflow run <workflow.json|->");

    FILE *f = NULL;
    int is_stdin = (strcmp(argv[2], "-") == 0);
    if (is_stdin) {
        f = stdin;
    } else {
        f = fopen(argv[2], "rb");
        if (!f)
            return emit_simple_failure("INVALID_WORKFLOW",
                                       "cannot open workflow document");
    }

    size_t len = 0;
    char *text = read_all(f, &len);
    if (!is_stdin)
        fclose(f);
    if (!text)
        return emit_simple_failure("INVALID_WORKFLOW",
                                   "cannot read workflow document");

    P p;
    memset(&p, 0, sizeof p);
    /* tolerate a UTF-8 BOM before the document */
    if (len >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
        p.s = text + 3;
        p.len = len - 3;
    } else {
        p.s = text;
        p.len = len;
    }
    Value *doc = parse_value(&p, 0);
    if (doc) {
        skip_ws(&p);
        if (p.pos != p.len) {
            perr(&p, "trailing content after JSON document");
            doc = NULL;
        }
    }
    if (!doc) {
        char msg[320];
        snprintf(msg, sizeof msg, "invalid JSON: %s",
                 p.err[0] ? p.err : "parse error");
        return emit_simple_failure("INVALID_WORKFLOW", msg);
    }

    Value *resp = NULL;
    int rc = execute(doc, &resp);
    emit_response(resp);
    return rc;
}
