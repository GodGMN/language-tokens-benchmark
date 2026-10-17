/*
 * workflow.c -- declarative workflow engine (C11)
 *
 * Implements the contract in SPEC.md: a JSON workflow document is read,
 * structurally validated, and executed with topological ordering,
 * $ref resolution, conditions and retries.
 *
 * No external dependencies beyond the C standard library.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>
#include <errno.h>
#include <limits.h>
#include <inttypes.h>

/* ------------------------------------------------------------------ */
/* memory helpers (process is short lived; leaks are acceptable)       */
/* ------------------------------------------------------------------ */

static void oom(void)
{
    fputs("{\"status\":\"failed\",\"results\":{},\"execution_order\":[],"
          "\"skipped\":[],\"error\":{\"code\":\"INTERNAL_ERROR\","
          "\"step\":null,\"message\":\"out of memory\"}}\n", stdout);
    fflush(stdout);
    exit(3);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) oom();
    return p;
}

static void *xcalloc(size_t n, size_t s)
{
    void *p = calloc(n ? n : 1, s ? s : 1);
    if (!p) oom();
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) oom();
    return q;
}

/* ------------------------------------------------------------------ */
/* growable byte buffer                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    char *p;
    size_t n, cap;
} Buf;

static void buf_init(Buf *b, size_t cap)
{
    b->cap = cap ? cap : 32;
    b->p = xmalloc(b->cap);
    b->n = 0;
    b->p[0] = 0;
}

static void buf_reserve(Buf *b, size_t extra)
{
    if (b->n + extra + 1 > b->cap) {
        size_t nc = b->cap * 2;
        while (nc < b->n + extra + 1) nc *= 2;
        b->p = xrealloc(b->p, nc);
        b->cap = nc;
    }
}

static void buf_put(Buf *b, const char *s, size_t n)
{
    buf_reserve(b, n);
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void buf_puts(Buf *b, const char *s)
{
    buf_put(b, s, strlen(s));
}

static void buf_putc(Buf *b, char c)
{
    buf_reserve(b, 1);
    b->p[b->n++] = c;
    b->p[b->n] = 0;
}

static void buf_printf(Buf *b, const char *fmt, ...)
{
    va_list ap;
    char tmp[256];
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    buf_puts(b, tmp);
}

/* ------------------------------------------------------------------ */
/* error state                                                         */
/* ------------------------------------------------------------------ */

static const char *g_errcode = NULL;
static const char *g_errstep = NULL;
static char g_errmsg[512];

static void seterr(const char *code, const char *fmt, ...)
{
    va_list ap;
    char tmp[512];
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    g_errcode = code;
    memcpy(g_errmsg, tmp, sizeof g_errmsg);
}

static void verr(const char *code, const char *step, const char *fmt, ...)
{
    va_list ap;
    char tmp[512];
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    g_errcode = code;
    g_errstep = step;
    memcpy(g_errmsg, tmp, sizeof g_errmsg);
}

/* ------------------------------------------------------------------ */
/* JSON values                                                         */
/* ------------------------------------------------------------------ */

typedef enum { J_NULL, J_BOOL, J_INT, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JVal JVal;

struct JVal {
    JType t;
    int b;              /* J_BOOL */
    int num_is_int;     /* J_NUM: original literal was an integer */
    int64_t i;          /* J_INT */
    double d;           /* J_NUM */
    char *s;            /* J_STR (and raw literal text for J_NUM) */
    size_t slen;
    JVal **items;       /* J_ARR */
    size_t n, cap;
    char **keys;        /* J_OBJ */
    size_t *klens;
    JVal **vals;
    size_t on, ocap;
};

static JVal *jnew(JType t)
{
    JVal *v = xcalloc(1, sizeof *v);
    v->t = t;
    return v;
}

static JVal *jnull(void) { return jnew(J_NULL); }

static JVal *jbool(int b)
{
    JVal *v = jnew(J_BOOL);
    v->b = b ? 1 : 0;
    return v;
}

static JVal *jint(int64_t i)
{
    JVal *v = jnew(J_INT);
    v->i = i;
    return v;
}

static JVal *jnum(double d)
{
    JVal *v = jnew(J_NUM);
    v->d = d;
    return v;
}

static JVal *jstrn(const char *s, size_t n)
{
    JVal *v = jnew(J_STR);
    v->s = xmalloc(n + 1);
    memcpy(v->s, s, n);
    v->s[n] = 0;
    v->slen = n;
    return v;
}

static JVal *jstr(const char *s) { return jstrn(s, strlen(s)); }

static JVal *jarr(void)
{
    JVal *v = jnew(J_ARR);
    return v;
}

static void jarr_push(JVal *a, JVal *v)
{
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 4;
        a->items = xrealloc(a->items, a->cap * sizeof *a->items);
    }
    a->items[a->n++] = v;
}

static JVal *jobj(void)
{
    JVal *v = jnew(J_OBJ);
    return v;
}

static JVal *jobj_setn(JVal *o, const char *k, size_t kl, JVal *v)
{
    size_t i;
    for (i = 0; i < o->on; i++) {
        if (o->klens[i] == kl && memcmp(o->keys[i], k, kl) == 0) {
            o->vals[i] = v;
            return o;
        }
    }
    if (o->on == o->ocap) {
        o->ocap = o->ocap ? o->ocap * 2 : 4;
        o->keys = xrealloc(o->keys, o->ocap * sizeof *o->keys);
        o->klens = xrealloc(o->klens, o->ocap * sizeof *o->klens);
        o->vals = xrealloc(o->vals, o->ocap * sizeof *o->vals);
    }
    o->keys[o->on] = xmalloc(kl + 1);
    memcpy(o->keys[o->on], k, kl);
    o->keys[o->on][kl] = 0;
    o->klens[o->on] = kl;
    o->vals[o->on] = v;
    o->on++;
    return o;
}

static JVal *jobj_getn(JVal *o, const char *k, size_t kl)
{
    size_t i;
    if (!o || o->t != J_OBJ) return NULL;
    for (i = 0; i < o->on; i++) {
        if (o->klens[i] == kl && memcmp(o->keys[i], k, kl) == 0)
            return o->vals[i];
    }
    return NULL;
}

static JVal *jobj_get(JVal *o, const char *k)
{
    return jobj_getn(o, k, strlen(k));
}

static int jisnum(const JVal *v) { return v->t == J_INT || v->t == J_NUM; }

static const char *type_name(const JVal *v)
{
    switch (v->t) {
    case J_NULL: return "null";
    case J_BOOL: return "boolean";
    case J_INT:
    case J_NUM: return "number";
    case J_STR: return "string";
    case J_ARR: return "array";
    case J_OBJ: return "object";
    }
    return "null";
}

/* ------------------------------------------------------------------ */
/* serializer                                                          */
/* ------------------------------------------------------------------ */

static void json_escape(Buf *b, const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
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
            if (c < 0x20) buf_printf(b, "\\u%04x", (unsigned)c);
            else buf_putc(b, (char)c);
        }
    }
}

static void ser(Buf *b, JVal *v);

static void format_double(char *out, size_t outsz, double d)
{
    char sci[64];
    char digits[32];
    size_t nd = 0, o = 0, i;
    int p, E = 0, neg = 0;

    if (!isfinite(d)) {
        snprintf(out, outsz, "null");
        return;
    }
    if (d == 0.0) {
        snprintf(out, outsz, signbit(d) ? "-0.0" : "0.0");
        return;
    }
    /* shortest representation that round-trips to the same double */
    for (p = 1; p <= 17; p++) {
        snprintf(sci, sizeof sci, "%.*e", p - 1, d);
        if (strtod(sci, NULL) == d) break;
    }
    {
        const char *s = sci;
        if (*s == '-') {
            neg = 1;
            s++;
        }
        while (*s && *s != 'e' && *s != 'E') {
            if (*s >= '0' && *s <= '9' && nd + 1 < sizeof digits)
                digits[nd++] = *s;
            s++;
        }
        if (*s == 'e' || *s == 'E') E = atoi(s + 1);
    }
    while (nd > 1 && digits[nd - 1] == '0') nd--;
    if (neg && o + 1 < outsz) out[o++] = '-';

    if (E < -4 || E >= 16) {
        /* scientific notation, exponent with sign and at least two digits */
        int ae = E < 0 ? -E : E;
        if (o + 1 < outsz) out[o++] = digits[0];
        if (nd > 1) {
            if (o + 1 < outsz) out[o++] = '.';
            for (i = 1; i < nd && o + 1 < outsz; i++) out[o++] = digits[i];
        }
        if (o + 5 < outsz)
            o += (size_t)snprintf(out + o, outsz - o, "e%c%02d",
                                  E < 0 ? '-' : '+', ae);
    } else if (E >= 0) {
        size_t intdigits = (size_t)E + 1;
        size_t k;
        for (k = 0; k < intdigits; k++)
            if (o + 1 < outsz) out[o++] = k < nd ? digits[k] : '0';
        if (nd > intdigits) {
            if (o + 1 < outsz) out[o++] = '.';
            for (k = intdigits; k < nd && o + 1 < outsz; k++)
                out[o++] = digits[k];
        } else if (o + 2 < outsz) {
            out[o++] = '.';
            out[o++] = '0';
        }
    } else {
        size_t zeros = (size_t)(-E) - 1;
        size_t k;
        if (o + 2 < outsz) {
            out[o++] = '0';
            out[o++] = '.';
        }
        for (k = 0; k < zeros && o + 1 < outsz; k++) out[o++] = '0';
        for (i = 0; i < nd && o + 1 < outsz; i++) out[o++] = digits[i];
    }
    out[o] = 0;
}

static void ser_number(Buf *b, JVal *v)
{
    if (v->t == J_INT) {
        buf_printf(b, "%" PRId64, v->i);
    } else if (v->s && (v->num_is_int || !isfinite(v->d))) {
        /* integer literals outside i64 (and out-of-range doubles) are
           preserved exactly instead of being rounded or replaced */
        buf_put(b, v->s, v->slen);
    } else {
        char tmp[384];
        format_double(tmp, sizeof tmp, v->d);
        buf_puts(b, tmp);
    }
}

static void ser(Buf *b, JVal *v)
{
    size_t i;
    switch (v->t) {
    case J_NULL: buf_puts(b, "null"); break;
    case J_BOOL: buf_puts(b, v->b ? "true" : "false"); break;
    case J_INT:
    case J_NUM: ser_number(b, v); break;
    case J_STR:
        buf_putc(b, '"');
        json_escape(b, v->s, v->slen);
        buf_putc(b, '"');
        break;
    case J_ARR:
        buf_putc(b, '[');
        for (i = 0; i < v->n; i++) {
            if (i) buf_putc(b, ',');
            ser(b, v->items[i]);
        }
        buf_putc(b, ']');
        break;
    case J_OBJ:
        buf_putc(b, '{');
        for (i = 0; i < v->on; i++) {
            if (i) buf_putc(b, ',');
            buf_putc(b, '"');
            json_escape(b, v->keys[i], v->klens[i]);
            buf_puts(b, "\":");
            ser(b, v->vals[i]);
        }
        buf_putc(b, '}');
        break;
    }
}

/* ------------------------------------------------------------------ */
/* ordering / equality                                                 */
/* ------------------------------------------------------------------ */

static int type_rank(const JVal *v)
{
    switch (v->t) {
    case J_INT:
    case J_NUM: return 0;
    case J_STR: return 1;
    case J_BOOL: return 2;
    case J_NULL: return 3;
    case J_ARR: return 4;
    case J_OBJ: return 5;
    }
    return 6;
}

static long double numval(const JVal *v)
{
    return v->t == J_INT ? (long double)v->i : (long double)v->d;
}

static int is_big_int(const JVal *v)
{
    return v->t == J_NUM && v->num_is_int && v->s && v->slen > 0;
}

static int bigint_cmp(const char *a, size_t an, const char *b, size_t bn)
{
    int aneg = a[0] == '-';
    int bneg = b[0] == '-';
    const char *ad = a + (aneg ? 1 : 0);
    const char *bd = b + (bneg ? 1 : 0);
    size_t al = an - (size_t)(aneg ? 1 : 0);
    size_t bl = bn - (size_t)(bneg ? 1 : 0);
    int mag;
    while (al > 1 && *ad == '0') { ad++; al--; }
    while (bl > 1 && *bd == '0') { bd++; bl--; }
    if (al == 1 && *ad == '0') aneg = 0;
    if (bl == 1 && *bd == '0') bneg = 0;
    if (aneg != bneg) return aneg ? -1 : 1;
    if (al != bl) mag = al < bl ? -1 : 1;
    else {
        int c = memcmp(ad, bd, al);
        mag = c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    return aneg ? -mag : mag;
}

static int numcmp(const JVal *a, const JVal *b)
{
    if (a->t == J_INT && b->t == J_INT)
        return a->i < b->i ? -1 : (a->i > b->i ? 1 : 0);
    {
        int abig = is_big_int(a);
        int bbig = is_big_int(b);
        if (abig && bbig) return bigint_cmp(a->s, a->slen, b->s, b->slen);
        if (a->t == J_INT && bbig) return b->s[0] == '-' ? 1 : -1;
        if (abig && b->t == J_INT) return a->s[0] == '-' ? -1 : 1;
    }
    {
        long double x = numval(a), y = numval(b);
        return x < y ? -1 : (x > y ? 1 : 0);
    }
}

static int strcmp_l(const char *a, size_t an, const char *b, size_t bn)
{
    size_t n = an < bn ? an : bn;
    int c = n ? memcmp(a, b, n) : 0;
    if (c) return c < 0 ? -1 : 1;
    return an < bn ? -1 : (an > bn ? 1 : 0);
}

static int keycmp_idx(const JVal *o, size_t a, size_t b)
{
    return strcmp_l(o->keys[a], o->klens[a], o->keys[b], o->klens[b]);
}

static void sort_key_idx(const JVal *o, size_t *idx)
{
    size_t i, j;
    for (i = 0; i < o->on; i++) idx[i] = i;
    for (i = 1; i < o->on; i++) {
        size_t x = idx[i];
        j = i;
        while (j > 0 && keycmp_idx(o, idx[j - 1], x) > 0) {
            idx[j] = idx[j - 1];
            j--;
        }
        idx[j] = x;
    }
}

static int valcmp(JVal *a, JVal *b)
{
    int ra = type_rank(a), rb = type_rank(b);
    if (ra != rb) return ra < rb ? -1 : 1;
    switch (a->t) {
    case J_NULL:
        return 0;
    case J_BOOL:
        return a->b < b->b ? -1 : (a->b > b->b ? 1 : 0);
    case J_INT:
    case J_NUM:
        return numcmp(a, b);
    case J_STR:
        return strcmp_l(a->s, a->slen, b->s, b->slen);
    case J_ARR: {
        size_t i, n = a->n < b->n ? a->n : b->n;
        for (i = 0; i < n; i++) {
            int c = valcmp(a->items[i], b->items[i]);
            if (c) return c;
        }
        return a->n < b->n ? -1 : (a->n > b->n ? 1 : 0);
    }
    case J_OBJ: {
        size_t i;
        if (a->on == 0 || b->on == 0) {
            if (a->on == b->on) return 0;
            return a->on < b->on ? -1 : 1;
        }
        {
            size_t *ia = xmalloc(a->on * sizeof *ia);
            size_t *ib = xmalloc(b->on * sizeof *ib);
            int res = 0;
            sort_key_idx(a, ia);
            sort_key_idx(b, ib);
            {
                size_t n = a->on < b->on ? a->on : b->on;
                for (i = 0; i < n; i++) {
                    int c = strcmp_l(a->keys[ia[i]], a->klens[ia[i]],
                                     b->keys[ib[i]], b->klens[ib[i]]);
                    if (c) { res = c; goto done; }
                }
                if (a->on != b->on) {
                    res = a->on < b->on ? -1 : 1;
                    goto done;
                }
                for (i = 0; i < n; i++) {
                    int c = valcmp(a->vals[ia[i]], b->vals[ib[i]]);
                    if (c) { res = c; goto done; }
                }
            }
        done:
            free(ia);
            free(ib);
            return res;
        }
    }
    }
    return 0;
}

static int valeq(JVal *a, JVal *b)
{
    if (jisnum(a) && jisnum(b)) return numcmp(a, b) == 0;
    if (a->t != b->t) return 0;
    switch (a->t) {
    case J_NULL: return 1;
    case J_BOOL: return a->b == b->b;
    case J_STR: return strcmp_l(a->s, a->slen, b->s, b->slen) == 0;
    case J_ARR: {
        size_t i;
        if (a->n != b->n) return 0;
        for (i = 0; i < a->n; i++)
            if (!valeq(a->items[i], b->items[i])) return 0;
        return 1;
    }
    case J_OBJ: {
        size_t i;
        if (a->on != b->on) return 0;
        for (i = 0; i < a->on; i++) {
            JVal *bv = jobj_getn(b, a->keys[i], a->klens[i]);
            if (!bv) return 0;
            if (!valeq(a->vals[i], bv)) return 0;
        }
        return 1;
    }
    default:
        return 0;
    }
}

static int truthy(JVal *v)
{
    switch (v->t) {
    case J_NULL: return 0;
    case J_BOOL: return v->b;
    case J_INT: return v->i != 0;
    case J_NUM: return v->d != 0.0;
    case J_STR: return v->slen > 0;
    case J_ARR: return v->n > 0;
    case J_OBJ: return v->on > 0;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* JSON parser                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *p, *end;
    int depth;
    char msg[160];
} P;

static void perr(P *ps, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ps->msg, sizeof ps->msg, fmt, ap);
    va_end(ap);
}

static void pskip(P *ps)
{
    while (ps->p < ps->end) {
        char c = *ps->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ps->p++;
        else break;
    }
}

static JVal *parse_value(P *ps);

static void utf8_enc(Buf *b, uint32_t cp)
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

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex4(const char *p)
{
    int i, v = 0;
    for (i = 0; i < 4; i++) {
        int h = hexval(p[i]);
        if (h < 0) return -1;
        v = v * 16 + h;
    }
    return v;
}

static JVal *parse_string(P *ps)
{
    const char *p = ps->p, *end = ps->end;
    Buf b;
    if (p >= end || *p != '"') {
        perr(ps, "expected string");
        return NULL;
    }
    p++;
    buf_init(&b, 16);
    while (p < end) {
        unsigned char c = (unsigned char)*p;
        if (c == '"') {
            p++;
            ps->p = p;
            return jstrn(b.p, b.n);
        }
        if (c == '\\') {
            char e;
            p++;
            if (p >= end) {
                perr(ps, "bad escape");
                return NULL;
            }
            e = *p++;
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
                int v;
                uint32_t cp;
                if (p + 4 > end) {
                    perr(ps, "bad \\u escape");
                    return NULL;
                }
                v = hex4(p);
                if (v < 0) {
                    perr(ps, "bad \\u escape");
                    return NULL;
                }
                p += 4;
                cp = (uint32_t)v;
                if (cp >= 0xD800 && cp <= 0xDBFF && p + 6 <= end &&
                    p[0] == '\\' && p[1] == 'u') {
                    int lo = hex4(p + 2);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000u + ((cp - 0xD800u) << 10) +
                             ((uint32_t)lo - 0xDC00u);
                        p += 6;
                    }
                }
                if (cp >= 0xD800 && cp <= 0xDFFF) {
                    perr(ps, "lone surrogate is not valid UTF-8");
                    return NULL;
                }
                utf8_enc(&b, cp);
                break;
            }
            default:
                perr(ps, "bad escape character");
                return NULL;
            }
        } else if (c < 0x20) {
            perr(ps, "control character in string");
            return NULL;
        } else if (c < 0x80) {
            buf_putc(&b, (char)c);
            p++;
        } else {
            /* validate a multi-byte UTF-8 sequence */
            size_t need;
            unsigned char c1;
            if (c >= 0xC2 && c <= 0xDF) need = 1;
            else if (c >= 0xE0 && c <= 0xEF) need = 2;
            else if (c >= 0xF0 && c <= 0xF4) need = 3;
            else {
                perr(ps, "invalid UTF-8 byte in string");
                return NULL;
            }
            if ((size_t)(end - p) <= need) {
                perr(ps, "truncated UTF-8 sequence in string");
                return NULL;
            }
            c1 = (unsigned char)p[1];
            if ((c1 & 0xC0) != 0x80) {
                perr(ps, "invalid UTF-8 continuation byte");
                return NULL;
            }
            if (c == 0xE0 && c1 < 0xA0) {
                perr(ps, "overlong UTF-8 sequence");
                return NULL;
            }
            if (c == 0xED && c1 > 0x9F) {
                perr(ps, "UTF-8 surrogate is not valid");
                return NULL;
            }
            if (c == 0xF0 && c1 < 0x90) {
                perr(ps, "overlong UTF-8 sequence");
                return NULL;
            }
            if (c == 0xF4 && c1 > 0x8F) {
                perr(ps, "UTF-8 code point out of range");
                return NULL;
            }
            {
                size_t k;
                for (k = 2; k <= need; k++) {
                    if (((unsigned char)p[k] & 0xC0) != 0x80) {
                        perr(ps, "invalid UTF-8 continuation byte");
                        return NULL;
                    }
                }
            }
            buf_put(&b, p, need + 1);
            p += need + 1;
        }
    }
    perr(ps, "unterminated string");
    return NULL;
}

static JVal *parse_number(P *ps)
{
    const char *start = ps->p, *p = ps->p, *end = ps->end;
    int isint = 1;
    size_t len;

    if (p < end && *p == '-') p++;
    if (p >= end) {
        perr(ps, "bad number");
        return NULL;
    }
    if (*p == '0') {
        p++;
    } else if (*p >= '1' && *p <= '9') {
        while (p < end && *p >= '0' && *p <= '9') p++;
    } else {
        perr(ps, "bad number");
        return NULL;
    }
    if (p < end && *p == '.') {
        isint = 0;
        p++;
        if (!(p < end && *p >= '0' && *p <= '9')) {
            perr(ps, "bad fraction");
            return NULL;
        }
        while (p < end && *p >= '0' && *p <= '9') p++;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        isint = 0;
        p++;
        if (p < end && (*p == '+' || *p == '-')) p++;
        if (!(p < end && *p >= '0' && *p <= '9')) {
            perr(ps, "bad exponent");
            return NULL;
        }
        while (p < end && *p >= '0' && *p <= '9') p++;
    }
    len = (size_t)(p - start);
    ps->p = p;
    if (isint && len < 64) {
        char tmp[64];
        char *e;
        long long v;
        memcpy(tmp, start, len);
        tmp[len] = 0;
        errno = 0;
        v = strtoll(tmp, &e, 10);
        if (errno == 0 && *e == 0) return jint((int64_t)v);
    }
    {
        char *tmp = xmalloc(len + 1);
        double d;
        JVal *v;
        memcpy(tmp, start, len);
        tmp[len] = 0;
        d = strtod(tmp, NULL);
        free(tmp);
        v = jnum(d);
        v->num_is_int = isint;
        v->s = xmalloc(len + 1);
        memcpy(v->s, start, len);
        v->s[len] = 0;
        v->slen = len;
        return v;
    }
}

static JVal *parse_array(P *ps)
{
    JVal *arr = jarr();
    pskip(ps);
    if (ps->p >= ps->end || *ps->p != '[') {
        perr(ps, "expected array");
        return NULL;
    }
    ps->p++;
    if (++ps->depth > 256) {
        perr(ps, "nesting too deep");
        return NULL;
    }
    pskip(ps);
    if (ps->p < ps->end && *ps->p == ']') {
        ps->p++;
        ps->depth--;
        return arr;
    }
    for (;;) {
        JVal *v;
        pskip(ps);
        v = parse_value(ps);
        if (!v) return NULL;
        jarr_push(arr, v);
        pskip(ps);
        if (ps->p >= ps->end) {
            perr(ps, "unterminated array");
            return NULL;
        }
        if (*ps->p == ',') {
            ps->p++;
            continue;
        }
        if (*ps->p == ']') {
            ps->p++;
            ps->depth--;
            return arr;
        }
        perr(ps, "expected ',' or ']'");
        return NULL;
    }
}

static JVal *parse_object(P *ps)
{
    JVal *obj = jobj();
    pskip(ps);
    if (ps->p >= ps->end || *ps->p != '{') {
        perr(ps, "expected object");
        return NULL;
    }
    ps->p++;
    if (++ps->depth > 256) {
        perr(ps, "nesting too deep");
        return NULL;
    }
    pskip(ps);
    if (ps->p < ps->end && *ps->p == '}') {
        ps->p++;
        ps->depth--;
        return obj;
    }
    for (;;) {
        JVal *k, *v;
        pskip(ps);
        k = parse_string(ps);
        if (!k) return NULL;
        pskip(ps);
        if (ps->p >= ps->end || *ps->p != ':') {
            perr(ps, "expected ':'");
            return NULL;
        }
        ps->p++;
        pskip(ps);
        v = parse_value(ps);
        if (!v) return NULL;
        jobj_setn(obj, k->s, k->slen, v);
        pskip(ps);
        if (ps->p >= ps->end) {
            perr(ps, "unterminated object");
            return NULL;
        }
        if (*ps->p == ',') {
            ps->p++;
            continue;
        }
        if (*ps->p == '}') {
            ps->p++;
            ps->depth--;
            return obj;
        }
        perr(ps, "expected ',' or '}'");
        return NULL;
    }
}

static JVal *parse_value(P *ps)
{
    char c;
    pskip(ps);
    if (ps->p >= ps->end) {
        perr(ps, "unexpected end of input");
        return NULL;
    }
    c = *ps->p;
    if (c == '{') return parse_object(ps);
    if (c == '[') return parse_array(ps);
    if (c == '"') return parse_string(ps);
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(ps);
    if (ps->end - ps->p >= 4 && memcmp(ps->p, "true", 4) == 0) {
        ps->p += 4;
        return jbool(1);
    }
    if (ps->end - ps->p >= 5 && memcmp(ps->p, "false", 5) == 0) {
        ps->p += 5;
        return jbool(0);
    }
    if (ps->end - ps->p >= 4 && memcmp(ps->p, "null", 4) == 0) {
        ps->p += 4;
        return jnull();
    }
    perr(ps, "unexpected character 0x%02x", (unsigned)(unsigned char)c);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* step / engine structures                                            */
/* ------------------------------------------------------------------ */

typedef struct Step Step;

typedef JVal *(*OpFn)(Step *st, JVal *args);

struct Step {
    char *id;
    size_t idlen;
    OpFn fn;
    JVal *args;              /* NULL means empty object */
    JVal *when;              /* NULL means always run */
    JVal **depnames;         /* referenced string values from document */
    size_t *depnlens;
    size_t ndepnames;
    size_t *depidx;
    size_t ndeps;
    int max_attempts;
    int64_t fail_calls;
    /* runtime */
    int finished;
    int skipped;
    JVal *result;
};

typedef struct {
    Step *steps;
    size_t n;
    unsigned char *clo;      /* n*n transitive dependency matrix */
} Engine;

typedef struct {
    Engine *eng;
    JVal **res;
    unsigned char *finished;
    unsigned char *skipped;
} Ctx;

/* ------------------------------------------------------------------ */
/* small helpers for ops                                               */
/* ------------------------------------------------------------------ */

static JVal *jfail(const char *code, const char *fmt, ...)
{
    va_list ap;
    char tmp[512];
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    g_errcode = code;
    memcpy(g_errmsg, tmp, sizeof g_errmsg);
    return NULL;
}

static int ifail(const char *fmt, ...)
{
    va_list ap;
    char tmp[512];
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    g_errcode = "INVALID_ARG";
    memcpy(g_errmsg, tmp, sizeof g_errmsg);
    return 0;
}

static JVal *jifail(const char *fmt, ...)
{
    va_list ap;
    char tmp[512];
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    g_errcode = "INVALID_ARG";
    memcpy(g_errmsg, tmp, sizeof g_errmsg);
    return NULL;
}

static int arg_only(JVal *args, const char **allowed)
{
    size_t i;
    if (!args || args->t != J_OBJ) return ifail("args must be an object");
    for (i = 0; i < args->on; i++) {
        int ok = 0;
        const char **a;
        for (a = allowed; *a; a++) {
            size_t alen = strlen(*a);
            if (args->klens[i] == alen &&
                memcmp(args->keys[i], *a, alen) == 0) { ok = 1; break; }
        }
        if (!ok) return ifail("unknown argument '%s'", args->keys[i]);
    }
    return 1;
}

static JVal *arg_req(JVal *args, const char *k)
{
    JVal *v = jobj_get(args, k);
    if (!v) {
        ifail("missing required argument '%s'", k);
        return NULL;
    }
    return v;
}

static int arg_i64(JVal *args, const char *k, int64_t *out)
{
    JVal *v = arg_req(args, k);
    if (!v) return 0;
    if (v->t != J_INT) return ifail("argument '%s' must be an integer", k);
    *out = v->i;
    return 1;
}

static int arg_opt_i64(JVal *args, const char *k, int64_t *out, int *present)
{
    JVal *v = jobj_get(args, k);
    if (!v) {
        *present = 0;
        return 1;
    }
    if (v->t != J_INT) return ifail("argument '%s' must be an integer", k);
    *out = v->i;
    *present = 1;
    return 1;
}

static JVal *arg_str(JVal *args, const char *k)
{
    JVal *v = arg_req(args, k);
    if (!v) return NULL;
    if (v->t != J_STR) {
        ifail("argument '%s' must be a string", k);
        return NULL;
    }
    return v;
}

static JVal *arg_arr(JVal *args, const char *k)
{
    JVal *v = arg_req(args, k);
    if (!v) return NULL;
    if (v->t != J_ARR) {
        ifail("argument '%s' must be an array", k);
        return NULL;
    }
    return v;
}

static JVal *arg_obj(JVal *args, const char *k)
{
    JVal *v = arg_req(args, k);
    if (!v) return NULL;
    if (v->t != J_OBJ) {
        ifail("argument '%s' must be an object", k);
        return NULL;
    }
    return v;
}

/* arithmetic kinds */
enum { AR_ADD, AR_SUB, AR_MUL, AR_DIV, AR_MOD };

static int arith_i64(int kind, int64_t a, int64_t b, int64_t *out)
{
    int64_t r;
    switch (kind) {
    case AR_ADD:
        if (__builtin_add_overflow(a, b, &r)) {
            g_errcode = "ARITHMETIC_OVERFLOW";
            snprintf(g_errmsg, sizeof g_errmsg, "integer overflow in add");
            return 0;
        }
        break;
    case AR_SUB:
        if (__builtin_sub_overflow(a, b, &r)) {
            g_errcode = "ARITHMETIC_OVERFLOW";
            snprintf(g_errmsg, sizeof g_errmsg, "integer overflow in subtract");
            return 0;
        }
        break;
    case AR_MUL:
        if (__builtin_mul_overflow(a, b, &r)) {
            g_errcode = "ARITHMETIC_OVERFLOW";
            snprintf(g_errmsg, sizeof g_errmsg, "integer overflow in multiply");
            return 0;
        }
        break;
    case AR_DIV:
        if (b == 0) {
            g_errcode = "DIVISION_BY_ZERO";
            snprintf(g_errmsg, sizeof g_errmsg, "division by zero");
            return 0;
        }
        if (a == INT64_MIN && b == -1) {
            g_errcode = "ARITHMETIC_OVERFLOW";
            snprintf(g_errmsg, sizeof g_errmsg, "integer overflow in divide");
            return 0;
        }
        r = a / b;
        break;
    case AR_MOD:
        if (b == 0) {
            g_errcode = "DIVISION_BY_ZERO";
            snprintf(g_errmsg, sizeof g_errmsg, "modulo by zero");
            return 0;
        }
        if (a == INT64_MIN && b == -1) {
            g_errcode = "ARITHMETIC_OVERFLOW";
            snprintf(g_errmsg, sizeof g_errmsg, "integer overflow in mod");
            return 0;
        }
        r = a % b;
        break;
    default:
        return ifail("bad arithmetic kind"), 0;
    }
    *out = r;
    return 1;
}

static JVal *arith_apply(int kind, int64_t a, int64_t b)
{
    int64_t r;
    if (!arith_i64(kind, a, b, &r)) return NULL;
    return jint(r);
}

/* ------------------------------------------------------------------ */
/* math ops                                                            */
/* ------------------------------------------------------------------ */

static JVal *op_arith_common(JVal *args, int kind)
{
    int64_t a, b;
    if (!arg_only(args, (const char *[]){"a", "b", NULL})) return NULL;
    if (!arg_i64(args, "a", &a) || !arg_i64(args, "b", &b)) return NULL;
    return arith_apply(kind, a, b);
}

static JVal *op_add(Step *st, JVal *args) { (void)st; return op_arith_common(args, AR_ADD); }
static JVal *op_subtract(Step *st, JVal *args) { (void)st; return op_arith_common(args, AR_SUB); }
static JVal *op_multiply(Step *st, JVal *args) { (void)st; return op_arith_common(args, AR_MUL); }
static JVal *op_divide(Step *st, JVal *args) { (void)st; return op_arith_common(args, AR_DIV); }
static JVal *op_mod(Step *st, JVal *args) { (void)st; return op_arith_common(args, AR_MOD); }

static JVal *op_sum(Step *st, JVal *args)
{
    JVal *in;
    int64_t acc = 0;
    size_t i;
    (void)st;
    if (!arg_only(args, (const char *[]){"input", NULL})) return NULL;
    in = arg_arr(args, "input");
    if (!in) return NULL;
    for (i = 0; i < in->n; i++) {
        int64_t v;
        if (in->items[i]->t != J_INT)
            return jifail("sum input elements must be integers");
        v = in->items[i]->i;
        if (__builtin_add_overflow(acc, v, &acc))
            return jfail("ARITHMETIC_OVERFLOW", "integer overflow in sum");
    }
    return jint(acc);
}

static JVal *op_minmax(Step *st, JVal *args, int want_max)
{
    JVal *in;
    int64_t best;
    size_t i;
    (void)st;
    if (!arg_only(args, (const char *[]){"input", NULL})) return NULL;
    in = arg_arr(args, "input");
    if (!in) return NULL;
    if (in->n == 0)
        return jfail("EMPTY_ARRAY", "%s of empty array", want_max ? "max" : "min");
    if (in->items[0]->t != J_INT)
        return jifail("min/max input elements must be integers");
    best = in->items[0]->i;
    for (i = 1; i < in->n; i++) {
        int64_t v;
        if (in->items[i]->t != J_INT)
            return jifail("min/max input elements must be integers");
        v = in->items[i]->i;
        if (want_max ? (v > best) : (v < best)) best = v;
    }
    return jint(best);
}

static JVal *op_min(Step *st, JVal *args) { return op_minmax(st, args, 0); }
static JVal *op_max(Step *st, JVal *args) { return op_minmax(st, args, 1); }

#define RANGE_LIMIT 1000000

static JVal *op_range(Step *st, JVal *args)
{
    int64_t start, end;
    JVal *out;
    uint64_t diff;
    int64_t x;
    (void)st;
    if (!arg_only(args, (const char *[]){"start", "end", NULL})) return NULL;
    if (!arg_i64(args, "start", &start) || !arg_i64(args, "end", &end))
        return NULL;
    out = jarr();
    if (start > end) return out;
    /* diff is exact for start <= end; count == diff + 1. Compare against the
       limit without computing diff + 1, which could overflow. */
    diff = (uint64_t)end - (uint64_t)start;
    if (diff >= (uint64_t)RANGE_LIMIT)
        return jifail("range too large");
    for (x = start;; x++) {
        jarr_push(out, jint(x));
        if (x == end) break;
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* string ops                                                          */
/* ------------------------------------------------------------------ */

static int is_ascii_ws(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

static size_t u8_next(const char *s, size_t len, size_t i)
{
    unsigned char c = (unsigned char)s[i];
    size_t n = 1;
    if (c >= 0xF0) n = 4;
    else if (c >= 0xE0) n = 3;
    else if (c >= 0xC0) n = 2;
    if (i + n > len) n = 1;
    return i + n;
}

static long find_sub(const char *h, size_t hn, const char *n, size_t nn,
                     size_t from)
{
    size_t i;
    if (nn == 0) return from <= hn ? (long)from : -1;
    if (nn > hn) return -1;
    for (i = from; i + nn <= hn; i++) {
        if (h[i] == n[0] && memcmp(h + i, n, nn) == 0) return (long)i;
    }
    return -1;
}

static JVal *op_concat(Step *st, JVal *args)
{
    JVal *a, *b;
    Buf out;
    (void)st;
    if (!arg_only(args, (const char *[]){"a", "b", NULL})) return NULL;
    a = arg_str(args, "a");
    if (!a) return NULL;
    b = arg_str(args, "b");
    if (!b) return NULL;
    buf_init(&out, a->slen + b->slen + 1);
    buf_put(&out, a->s, a->slen);
    buf_put(&out, b->s, b->slen);
    return jstrn(out.p, out.n);
}

static JVal *op_split(Step *st, JVal *args)
{
    JVal *in, *sep, *out;
    size_t pos;
    (void)st;
    if (!arg_only(args, (const char *[]){"input", "sep", NULL})) return NULL;
    in = arg_str(args, "input");
    if (!in) return NULL;
    sep = arg_str(args, "sep");
    if (!sep) return NULL;
    out = jarr();
    if (sep->slen == 0) {
        pos = 0;
        while (pos < in->slen) {
            size_t nx = u8_next(in->s, in->slen, pos);
            jarr_push(out, jstrn(in->s + pos, nx - pos));
            pos = nx;
        }
        return out;
    }
    pos = 0;
    for (;;) {
        long idx = find_sub(in->s, in->slen, sep->s, sep->slen, pos);
        if (idx < 0) {
            jarr_push(out, jstrn(in->s + pos, in->slen - pos));
            break;
        }
        jarr_push(out, jstrn(in->s + pos, (size_t)idx - pos));
        pos = (size_t)idx + sep->slen;
    }
    return out;
}

static JVal *op_join(Step *st, JVal *args)
{
    JVal *in, *sep;
    Buf out;
    size_t i;
    (void)st;
    if (!arg_only(args, (const char *[]){"input", "sep", NULL})) return NULL;
    in = arg_arr(args, "input");
    if (!in) return NULL;
    sep = arg_str(args, "sep");
    if (!sep) return NULL;
    buf_init(&out, 32);
    for (i = 0; i < in->n; i++) {
        if (in->items[i]->t != J_STR)
            return jifail("join input elements must be strings");
        if (i) buf_put(&out, sep->s, sep->slen);
        buf_put(&out, in->items[i]->s, in->items[i]->slen);
    }
    return jstrn(out.p, out.n);
}

static JVal *op_uppercase(Step *st, JVal *args)
{
    JVal *in;
    size_t i;
    char *buf;
    (void)st;
    if (!arg_only(args, (const char *[]){"input", NULL})) return NULL;
    in = arg_str(args, "input");
    if (!in) return NULL;
    buf = xmalloc(in->slen + 1);
    memcpy(buf, in->s, in->slen + 1);
    for (i = 0; i < in->slen; i++)
        if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] = (char)(buf[i] - 'a' + 'A');
    return jstrn(buf, in->slen);
}

static JVal *op_lowercase(Step *st, JVal *args)
{
    JVal *in;
    size_t i;
    char *buf;
    (void)st;
    if (!arg_only(args, (const char *[]){"input", NULL})) return NULL;
    in = arg_str(args, "input");
    if (!in) return NULL;
    buf = xmalloc(in->slen + 1);
    memcpy(buf, in->s, in->slen + 1);
    for (i = 0; i < in->slen; i++)
        if (buf[i] >= 'A' && buf[i] <= 'Z') buf[i] = (char)(buf[i] - 'A' + 'a');
    return jstrn(buf, in->slen);
}

static JVal *op_trim(Step *st, JVal *args)
{
    JVal *in;
    size_t a = 0, b;
    (void)st;
    if (!arg_only(args, (const char *[]){"input", NULL})) return NULL;
    in = arg_str(args, "input");
    if (!in) return NULL;
    b = in->slen;
    while (a < b && is_ascii_ws((unsigned char)in->s[a])) a++;
    while (b > a && is_ascii_ws((unsigned char)in->s[b - 1])) b--;
    return jstrn(in->s + a, b - a);
}

static JVal *op_replace(Step *st, JVal *args)
{
    JVal *in, *search, *repl;
    Buf out;
    size_t pos = 0;
    (void)st;
    if (!arg_only(args, (const char *[]){"input", "search", "replace", NULL}))
        return NULL;
    in = arg_str(args, "input");
    if (!in) return NULL;
    search = arg_str(args, "search");
    if (!search) return NULL;
    repl = arg_str(args, "replace");
    if (!repl) return NULL;
    if (search->slen == 0) return jifail("replace search string must not be empty");
    buf_init(&out, in->slen + 16);
    for (;;) {
        long idx = find_sub(in->s, in->slen, search->s, search->slen, pos);
        if (idx < 0) {
            buf_put(&out, in->s + pos, in->slen - pos);
            break;
        }
        buf_put(&out, in->s + pos, (size_t)idx - pos);
        buf_put(&out, repl->s, repl->slen);
        pos = (size_t)idx + search->slen;
    }
    return jstrn(out.p, out.n);
}

static JVal *op_length(Step *st, JVal *args)
{
    JVal *in;
    size_t i = 0;
    int64_t n = 0;
    (void)st;
    if (!arg_only(args, (const char *[]){"input", NULL})) return NULL;
    in = arg_str(args, "input");
    if (!in) return NULL;
    while (i < in->slen) {
        i = u8_next(in->s, in->slen, i);
        n++;
    }
    return jint(n);
}

/* ------------------------------------------------------------------ */
/* array ops                                                           */
/* ------------------------------------------------------------------ */

typedef enum {
    TR_CONST, TR_ADD, TR_SUB, TR_MUL, TR_DIV, TR_MOD,
    TR_NEG, TR_ABS, TR_STRINGIFY, TR_TYPE, TR_OP
} TrKind;

typedef struct {
    TrKind k;
    JVal *cval;
    int64_t num;
    int opkind;
    int a_self, b_self;
    int64_t a_num, b_num;
} Transform;

static int is_self_ref(JVal *v)
{
    JVal *x;
    if (v->t != J_OBJ || v->on != 1) return 0;
    if (v->klens[0] != 5 || memcmp(v->keys[0], "$self", 5) != 0) return 0;
    x = v->vals[0];
    return x->t == J_BOOL && x->b;
}

static int tr_parse(JVal *t, Transform *tr)
{
    JVal *ty, *v;
    const char *s;
    memset(tr, 0, sizeof *tr);
    if (!t || t->t != J_OBJ) return ifail("transform must be an object");
    ty = jobj_get(t, "type");
    if (!ty || ty->t != J_STR) return ifail("transform requires a type");
    s = ty->s;
    if (strcmp(s, "constant") == 0) {
        if (!arg_only(t, (const char *[]){"type", "value", NULL})) return 0;
        v = jobj_get(t, "value");
        if (!v) return ifail("constant transform requires value");
        tr->k = TR_CONST;
        tr->cval = v;
        return 1;
    }
    if (strcmp(s, "negate") == 0 || strcmp(s, "abs") == 0 ||
        strcmp(s, "stringify") == 0 || strcmp(s, "type") == 0) {
        if (!arg_only(t, (const char *[]){"type", NULL})) return 0;
        if (strcmp(s, "negate") == 0) tr->k = TR_NEG;
        else if (strcmp(s, "abs") == 0) tr->k = TR_ABS;
        else if (strcmp(s, "stringify") == 0) tr->k = TR_STRINGIFY;
        else tr->k = TR_TYPE;
        return 1;
    }
    if (strcmp(s, "add") == 0 || strcmp(s, "subtract") == 0 ||
        strcmp(s, "multiply") == 0 || strcmp(s, "divide") == 0 ||
        strcmp(s, "mod") == 0) {
        if (!arg_only(t, (const char *[]){"type", "value", NULL})) return 0;
        if (!arg_i64(t, "value", &tr->num)) return 0;
        if (strcmp(s, "add") == 0) tr->k = TR_ADD;
        else if (strcmp(s, "subtract") == 0) tr->k = TR_SUB;
        else if (strcmp(s, "multiply") == 0) tr->k = TR_MUL;
        else if (strcmp(s, "divide") == 0) tr->k = TR_DIV;
        else tr->k = TR_MOD;
        return 1;
    }
    if (strcmp(s, "op") == 0) {
        JVal *opv, *av, *av2;
        if (!arg_only(t, (const char *[]){"type", "op", "args", NULL})) return 0;
        opv = jobj_get(t, "op");
        if (!opv || opv->t != J_STR) return ifail("op transform requires op");
        if (strcmp(opv->s, "add") == 0) tr->opkind = AR_ADD;
        else if (strcmp(opv->s, "subtract") == 0) tr->opkind = AR_SUB;
        else if (strcmp(opv->s, "multiply") == 0) tr->opkind = AR_MUL;
        else if (strcmp(opv->s, "divide") == 0) tr->opkind = AR_DIV;
        else if (strcmp(opv->s, "mod") == 0) tr->opkind = AR_MOD;
        else return ifail("unsupported op for map transform");
        av = jobj_get(t, "args");
        if (!av || av->t != J_OBJ) return ifail("op transform requires args object");
        if (!arg_only(av, (const char *[]){"a", "b", NULL})) return 0;
        av2 = jobj_get(av, "a");
        if (!av2) return ifail("missing argument 'a'");
        if (is_self_ref(av2)) tr->a_self = 1;
        else if (av2->t == J_INT) tr->a_num = av2->i;
        else return ifail("transform arg 'a' must be an integer or $self");
        av2 = jobj_get(av, "b");
        if (!av2) return ifail("missing argument 'b'");
        if (is_self_ref(av2)) tr->b_self = 1;
        else if (av2->t == J_INT) tr->b_num = av2->i;
        else return ifail("transform arg 'b' must be an integer or $self");
        tr->k = TR_OP;
        return 1;
    }
    return ifail("unknown transform type '%s'", s);
}

static JVal *tr_apply(Transform *tr, JVal *e)
{
    switch (tr->k) {
    case TR_CONST:
        return tr->cval;
    case TR_ADD:
    case TR_SUB:
    case TR_MUL:
    case TR_DIV:
    case TR_MOD: {
        int kind = tr->k == TR_ADD ? AR_ADD : tr->k == TR_SUB ? AR_SUB :
                   tr->k == TR_MUL ? AR_MUL : tr->k == TR_DIV ? AR_DIV : AR_MOD;
        if (e->t != J_INT) return jifail("map element must be an integer");
        return arith_apply(kind, e->i, tr->num);
    }
    case TR_NEG:
        if (e->t != J_INT) return jifail("negate element must be an integer");
        if (e->i == INT64_MIN)
            return jfail("ARITHMETIC_OVERFLOW", "integer overflow in negate");
        return jint(-e->i);
    case TR_ABS:
        if (e->t != J_INT) return jifail("abs element must be an integer");
        if (e->i == INT64_MIN)
            return jfail("ARITHMETIC_OVERFLOW", "integer overflow in abs");
        return jint(e->i < 0 ? -e->i : e->i);
    case TR_STRINGIFY: {
        Buf b;
        buf_init(&b, 32);
        ser(&b, e);
        return jstrn(b.p, b.n);
    }
    case TR_TYPE:
        return jstr(type_name(e));
    case TR_OP: {
        int64_t a, b;
        if (tr->a_self) {
            if (e->t != J_INT) return jifail("map element must be an integer");
            a = e->i;
        } else a = tr->a_num;
        if (tr->b_self) {
            if (e->t != J_INT) return jifail("map element must be an integer");
            b = e->i;
        } else b = tr->b_num;
        return arith_apply(tr->opkind, a, b);
    }
    }
    return jifail("bad transform");
}

static JVal *op_map(Step *st, JVal *args)
{
    JVal *in, *trv, *out;
    Transform tr;
    size_t i;
    (void)st;
    if (!arg_only(args, (const char *[]){"input", "transform", NULL})) return NULL;
    in = arg_arr(args, "input");
    if (!in) return NULL;
    trv = arg_req(args, "transform");
    if (!trv) return NULL;
    if (!tr_parse(trv, &tr)) return NULL;
    out = jarr();
    for (i = 0; i < in->n; i++) {
        JVal *v = tr_apply(&tr, in->items[i]);
        if (!v) return NULL;
        jarr_push(out, v);
    }
    return out;
}

typedef enum {
    PR_EVEN, PR_ODD, PR_GT, PR_GTE, PR_LT, PR_LTE, PR_EQ, PR_NE,
    PR_TRUTHY, PR_FALSY, PR_TYPE_IS, PR_CONTAINS
} PrKind;

typedef struct {
    PrKind k;
    int64_t num;
    char *s;
    size_t slen;
} Pred;

static int pred_parse(JVal *p, Pred *pr)
{
    JVal *ty, *v;
    const char *s;
    memset(pr, 0, sizeof *pr);
    if (!p || p->t != J_OBJ) return ifail("predicate must be an object");
    ty = jobj_get(p, "type");
    if (!ty || ty->t != J_STR) return ifail("predicate requires a type");
    s = ty->s;
    if (strcmp(s, "even") == 0 || strcmp(s, "odd") == 0 ||
        strcmp(s, "truthy") == 0 || strcmp(s, "falsy") == 0) {
        if (!arg_only(p, (const char *[]){"type", NULL})) return 0;
        if (strcmp(s, "even") == 0) pr->k = PR_EVEN;
        else if (strcmp(s, "odd") == 0) pr->k = PR_ODD;
        else if (strcmp(s, "truthy") == 0) pr->k = PR_TRUTHY;
        else pr->k = PR_FALSY;
        return 1;
    }
    if (strcmp(s, "gt") == 0 || strcmp(s, "gte") == 0 ||
        strcmp(s, "lt") == 0 || strcmp(s, "lte") == 0 ||
        strcmp(s, "eq") == 0 || strcmp(s, "ne") == 0) {
        if (!arg_only(p, (const char *[]){"type", "value", NULL})) return 0;
        if (!arg_i64(p, "value", &pr->num)) return 0;
        if (strcmp(s, "gt") == 0) pr->k = PR_GT;
        else if (strcmp(s, "gte") == 0) pr->k = PR_GTE;
        else if (strcmp(s, "lt") == 0) pr->k = PR_LT;
        else if (strcmp(s, "lte") == 0) pr->k = PR_LTE;
        else if (strcmp(s, "eq") == 0) pr->k = PR_EQ;
        else pr->k = PR_NE;
        return 1;
    }
    if (strcmp(s, "type_is") == 0) {
        static const char *names[] = {"null", "boolean", "number", "string",
                                      "array", "object", NULL};
        int ok = 0, i;
        if (!arg_only(p, (const char *[]){"type", "value", "type_name", NULL}))
            return 0;
        v = jobj_get(p, "value");
        if (!v) v = jobj_get(p, "type_name");
        if (!v || v->t != J_STR) return ifail("type_is requires a type name");
        for (i = 0; names[i]; i++)
            if (strcmp(names[i], v->s) == 0) ok = 1;
        if (!ok) return ifail("unknown type name '%s'", v->s);
        pr->k = PR_TYPE_IS;
        pr->s = v->s;
        pr->slen = v->slen;
        return 1;
    }
    if (strcmp(s, "string_contains") == 0) {
        if (!arg_only(p, (const char *[]){"type", "value", NULL})) return 0;
        v = jobj_get(p, "value");
        if (!v || v->t != J_STR) return ifail("string_contains requires a string value");
        pr->k = PR_CONTAINS;
        pr->s = v->s;
        pr->slen = v->slen;
        return 1;
    }
    return ifail("unknown predicate type '%s'", s);
}

static JVal *pred_apply(Pred *pr, JVal *e)
{
    switch (pr->k) {
    case PR_EVEN:
        if (e->t != J_INT) return jifail("even predicate requires integers");
        return jbool(e->i % 2 == 0);
    case PR_ODD:
        if (e->t != J_INT) return jifail("odd predicate requires integers");
        return jbool(e->i % 2 != 0);
    case PR_GT:
    case PR_GTE:
    case PR_LT:
    case PR_LTE:
    case PR_EQ:
    case PR_NE: {
        JVal needle;
        int c;
        memset(&needle, 0, sizeof needle);
        needle.t = J_INT;
        needle.i = pr->num;
        if (pr->k == PR_EQ) return jbool(valeq(e, &needle));
        if (pr->k == PR_NE) return jbool(!valeq(e, &needle));
        c = valcmp(e, &needle);
        if (pr->k == PR_GT) return jbool(c > 0);
        if (pr->k == PR_GTE) return jbool(c >= 0);
        if (pr->k == PR_LT) return jbool(c < 0);
        return jbool(c <= 0);
    }
    case PR_TRUTHY:
        return jbool(truthy(e));
    case PR_FALSY:
        return jbool(!truthy(e));
    case PR_TYPE_IS:
        return jbool(strcmp(type_name(e), pr->s) == 0);
    case PR_CONTAINS:
        if (e->t != J_STR) return jifail("string_contains requires string elements");
        return jbool(find_sub(e->s, e->slen, pr->s, pr->slen, 0) >= 0);
    }
    return jifail("bad predicate");
}

static JVal *op_filter(Step *st, JVal *args)
{
    JVal *in, *pv, *out;
    Pred pr;
    size_t i;
    (void)st;
    if (!arg_only(args, (const char *[]){"input", "predicate", NULL})) return NULL;
    in = arg_arr(args, "input");
    if (!in) return NULL;
    pv = arg_req(args, "predicate");
    if (!pv) return NULL;
    if (!pred_parse(pv, &pr)) return NULL;
    out = jarr();
    for (i = 0; i < in->n; i++) {
        JVal *r = pred_apply(&pr, in->items[i]);
        if (!r) return NULL;
        if (r->b) jarr_push(out, in->items[i]);
    }
    return out;
}

static JVal *op_reduce(Step *st, JVal *args)
{
    JVal *in, *opv;
    int kind;
    int64_t acc;
    int present;
    size_t i;
    (void)st;
    if (!arg_only(args, (const char *[]){"input", "op", "initial", NULL}))
        return NULL;
    in = arg_arr(args, "input");
    if (!in) return NULL;
    opv = arg_str(args, "op");
    if (!opv) return NULL;
    if (strcmp(opv->s, "add") == 0) kind = AR_ADD;
    else if (strcmp(opv->s, "subtract") == 0) kind = AR_SUB;
    else if (strcmp(opv->s, "multiply") == 0) kind = AR_MUL;
    else return jifail("reduce op must be add, subtract or multiply");
    if (!arg_opt_i64(args, "initial", &acc, &present)) return NULL;
    if (!present) acc = kind == AR_MUL ? 1 : 0;
    for (i = 0; i < in->n; i++) {
        int64_t next;
        if (in->items[i]->t != J_INT)
            return jifail("reduce input elements must be integers");
        if (!arith_i64(kind, acc, in->items[i]->i, &next)) return NULL;
        acc = next;
    }
    return jint(acc);
}

static void msort(JVal **a, JVal **tmp, size_t n)
{
    size_t m, i, j, k;
    if (n < 2) return;
    m = n / 2;
    msort(a, tmp, m);
    msort(a + m, tmp, n - m);
    i = 0; j = m; k = 0;
    while (i < m && j < n) {
        if (valcmp(a[j], a[i]) < 0) tmp[k++] = a[j++];
        else tmp[k++] = a[i++];
    }
    while (i < m) tmp[k++] = a[i++];
    while (j < n) tmp[k++] = a[j++];
    memcpy(a, tmp, n * sizeof *a);
}

static JVal *op_sort(Step *st, JVal *args)
{
    JVal *in, *out;
    JVal **tmp;
    (void)st;
    if (!arg_only(args, (const char *[]){"input", NULL})) return NULL;
    in = arg_arr(args, "input");
    if (!in) return NULL;
    out = jarr();
    if (in->n == 0) return out;
    out->items = xmalloc(in->n * sizeof *out->items);
    out->cap = in->n;
    memcpy(out->items, in->items, in->n * sizeof *out->items);
    out->n = in->n;
    tmp = xmalloc(in->n * sizeof *tmp);
    msort(out->items, tmp, out->n);
    free(tmp);
    return out;
}

static JVal *op_reverse(Step *st, JVal *args)
{
    JVal *in, *out;
    size_t i;
    (void)st;
    if (!arg_only(args, (const char *[]){"input", NULL})) return NULL;
    in = arg_arr(args, "input");
    if (!in) return NULL;
    out = jarr();
    for (i = in->n; i > 0; i--) jarr_push(out, in->items[i - 1]);
    return out;
}

static JVal *op_unique(Step *st, JVal *args)
{
    JVal *in, *out;
    size_t i, j;
    (void)st;
    if (!arg_only(args, (const char *[]){"input", NULL})) return NULL;
    in = arg_arr(args, "input");
    if (!in) return NULL;
    out = jarr();
    for (i = 0; i < in->n; i++) {
        int seen = 0;
        for (j = 0; j < out->n; j++) {
            if (valeq(out->items[j], in->items[i])) { seen = 1; break; }
        }
        if (!seen) jarr_push(out, in->items[i]);
    }
    return out;
}

static JVal *op_slice(Step *st, JVal *args)
{
    JVal *in, *out;
    int64_t start = 0, end = 0;
    int present = 0;
    int64_t len, i;
    (void)st;
    if (!arg_only(args, (const char *[]){"input", "start", "end", NULL}))
        return NULL;
    in = arg_arr(args, "input");
    if (!in) return NULL;
    if (!arg_i64(args, "start", &start)) return NULL;
    if (!arg_opt_i64(args, "end", &end, &present)) return NULL;
    len = (int64_t)in->n;
    if (!present) end = len;
    if (start < 0) {
        if (start < -len) start = 0;
        else start += len;
    } else if (start > len) start = len;
    if (end < 0) {
        if (end < -len) end = 0;
        else end += len;
    } else if (end > len) end = len;
    out = jarr();
    if (start >= end) return out;
    for (i = start; i < end; i++) jarr_push(out, in->items[i]);
    return out;
}

static JVal *op_flatten(Step *st, JVal *args)
{
    JVal *in, *out;
    size_t i, j;
    (void)st;
    if (!arg_only(args, (const char *[]){"input", NULL})) return NULL;
    in = arg_arr(args, "input");
    if (!in) return NULL;
    out = jarr();
    for (i = 0; i < in->n; i++) {
        if (in->items[i]->t == J_ARR) {
            for (j = 0; j < in->items[i]->n; j++)
                jarr_push(out, in->items[i]->items[j]);
        } else {
            jarr_push(out, in->items[i]);
        }
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* object ops                                                          */
/* ------------------------------------------------------------------ */

static JVal *copy_obj(JVal *o)
{
    JVal *r = jobj();
    size_t i;
    for (i = 0; i < o->on; i++)
        jobj_setn(r, o->keys[i], o->klens[i], o->vals[i]);
    return r;
}

static JVal *op_get(Step *st, JVal *args)
{
    JVal *in, *k, *v;
    (void)st;
    if (!arg_only(args, (const char *[]){"input", "key", NULL})) return NULL;
    in = arg_obj(args, "input");
    if (!in) return NULL;
    k = arg_str(args, "key");
    if (!k) return NULL;
    v = jobj_getn(in, k->s, k->slen);
    return v ? v : jnull();
}

static JVal *op_set(Step *st, JVal *args)
{
    JVal *in, *k, *v;
    (void)st;
    if (!arg_only(args, (const char *[]){"input", "key", "value", NULL}))
        return NULL;
    in = arg_obj(args, "input");
    if (!in) return NULL;
    k = arg_str(args, "key");
    if (!k) return NULL;
    v = arg_req(args, "value");
    if (!v) return NULL;
    return jobj_setn(copy_obj(in), k->s, k->slen, v);
}

static JVal *op_keys(Step *st, JVal *args)
{
    JVal *in, *out;
    size_t i, *idx;
    (void)st;
    if (!arg_only(args, (const char *[]){"input", NULL})) return NULL;
    in = arg_obj(args, "input");
    if (!in) return NULL;
    out = jarr();
    idx = xmalloc((in->on ? in->on : 1) * sizeof *idx);
    sort_key_idx(in, idx);
    for (i = 0; i < in->on; i++)
        jarr_push(out, jstrn(in->keys[idx[i]], in->klens[idx[i]]));
    free(idx);
    return out;
}

static JVal *op_values(Step *st, JVal *args)
{
    JVal *in, *out;
    size_t i, *idx;
    (void)st;
    if (!arg_only(args, (const char *[]){"input", NULL})) return NULL;
    in = arg_obj(args, "input");
    if (!in) return NULL;
    out = jarr();
    idx = xmalloc((in->on ? in->on : 1) * sizeof *idx);
    sort_key_idx(in, idx);
    for (i = 0; i < in->on; i++) jarr_push(out, in->vals[idx[i]]);
    free(idx);
    return out;
}

static JVal *op_merge(Step *st, JVal *args)
{
    JVal *a, *b, *out;
    size_t i;
    (void)st;
    if (!arg_only(args, (const char *[]){"a", "b", NULL})) return NULL;
    a = arg_obj(args, "a");
    if (!a) return NULL;
    b = arg_obj(args, "b");
    if (!b) return NULL;
    out = copy_obj(a);
    for (i = 0; i < b->on; i++)
        jobj_setn(out, b->keys[i], b->klens[i], b->vals[i]);
    return out;
}

static int key_in_list(JVal *k, JVal **list, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (list[i]->slen == k->slen && memcmp(list[i]->s, k->s, k->slen) == 0)
            return 1;
    return 0;
}

static JVal *op_pick(Step *st, JVal *args)
{
    JVal *in, *keys, *out;
    JVal **list;
    size_t i, n = 0;
    (void)st;
    if (!arg_only(args, (const char *[]){"input", "keys", NULL})) return NULL;
    in = arg_obj(args, "input");
    if (!in) return NULL;
    keys = arg_arr(args, "keys");
    if (!keys) return NULL;
    list = xmalloc((keys->n ? keys->n : 1) * sizeof *list);
    for (i = 0; i < keys->n; i++) {
        if (keys->items[i]->t != J_STR)
            return jifail("pick keys must be strings");
        list[n++] = keys->items[i];
    }
    /* sort requested keys byte-wise and deduplicate */
    for (i = 1; i < n; i++) {
        JVal *x = list[i];
        size_t j = i;
        while (j > 0 && strcmp_l(list[j - 1]->s, list[j - 1]->slen, x->s,
                                  x->slen) > 0) {
            list[j] = list[j - 1];
            j--;
        }
        list[j] = x;
    }
    out = jobj();
    for (i = 0; i < n; i++) {
        JVal *v;
        if (i > 0 && strcmp_l(list[i - 1]->s, list[i - 1]->slen, list[i]->s,
                               list[i]->slen) == 0)
            continue;
        v = jobj_getn(in, list[i]->s, list[i]->slen);
        jobj_setn(out, list[i]->s, list[i]->slen, v ? v : jnull());
    }
    free(list);
    return out;
}

static JVal *op_omit(Step *st, JVal *args)
{
    JVal *in, *keys, *out;
    JVal **list;
    size_t i, n = 0;
    (void)st;
    if (!arg_only(args, (const char *[]){"input", "keys", NULL})) return NULL;
    in = arg_obj(args, "input");
    if (!in) return NULL;
    keys = arg_arr(args, "keys");
    if (!keys) return NULL;
    list = xmalloc((keys->n ? keys->n : 1) * sizeof *list);
    for (i = 0; i < keys->n; i++) {
        if (keys->items[i]->t != J_STR)
            return jifail("omit keys must be strings");
        list[n++] = keys->items[i];
    }
    out = jobj();
    for (i = 0; i < in->on; i++) {
        JVal tmp;
        memset(&tmp, 0, sizeof tmp);
        tmp.t = J_STR;
        tmp.s = in->keys[i];
        tmp.slen = in->klens[i];
        if (key_in_list(&tmp, list, n)) continue;
        jobj_setn(out, in->keys[i], in->klens[i], in->vals[i]);
    }
    free(list);
    return out;
}

/* ------------------------------------------------------------------ */
/* utility ops                                                         */
/* ------------------------------------------------------------------ */

static JVal *op_constant(Step *st, JVal *args)
{
    JVal *v;
    (void)st;
    if (!arg_only(args, (const char *[]){"value", NULL})) return NULL;
    v = arg_req(args, "value");
    if (!v) return NULL;
    return v;
}

static JVal *op_equals(Step *st, JVal *args)
{
    JVal *a, *b;
    (void)st;
    if (!arg_only(args, (const char *[]){"a", "b", NULL})) return NULL;
    a = arg_req(args, "a");
    if (!a) return NULL;
    b = arg_req(args, "b");
    if (!b) return NULL;
    return jbool(valeq(a, b));
}

static JVal *op_compare(Step *st, JVal *args)
{
    JVal *a, *b, *opv;
    int c;
    (void)st;
    if (!arg_only(args, (const char *[]){"a", "b", "op", NULL})) return NULL;
    a = arg_req(args, "a");
    if (!a) return NULL;
    b = arg_req(args, "b");
    if (!b) return NULL;
    opv = arg_str(args, "op");
    if (!opv) return NULL;
    c = valcmp(a, b);
    if (strcmp(opv->s, "gt") == 0) return jbool(c > 0);
    if (strcmp(opv->s, "gte") == 0) return jbool(c >= 0);
    if (strcmp(opv->s, "lt") == 0) return jbool(c < 0);
    if (strcmp(opv->s, "lte") == 0) return jbool(c <= 0);
    return jifail("compare op must be gt, gte, lt or lte");
}

static JVal *op_and(Step *st, JVal *args)
{
    JVal *a, *b;
    (void)st;
    if (!arg_only(args, (const char *[]){"a", "b", NULL})) return NULL;
    a = arg_req(args, "a");
    if (!a) return NULL;
    b = arg_req(args, "b");
    if (!b) return NULL;
    return jbool(truthy(a) && truthy(b));
}

static JVal *op_or(Step *st, JVal *args)
{
    JVal *a, *b;
    (void)st;
    if (!arg_only(args, (const char *[]){"a", "b", NULL})) return NULL;
    a = arg_req(args, "a");
    if (!a) return NULL;
    b = arg_req(args, "b");
    if (!b) return NULL;
    return jbool(truthy(a) || truthy(b));
}

static JVal *op_not(Step *st, JVal *args)
{
    JVal *a;
    (void)st;
    if (!arg_only(args, (const char *[]){"a", NULL})) return NULL;
    a = arg_req(args, "a");
    if (!a) return NULL;
    return jbool(!truthy(a));
}

static JVal *op_type(Step *st, JVal *args)
{
    JVal *v;
    (void)st;
    if (!arg_only(args, (const char *[]){"value", NULL})) return NULL;
    v = arg_req(args, "value");
    if (!v) return NULL;
    return jstr(type_name(v));
}

static JVal *op_fail_until(Step *st, JVal *args)
{
    int64_t failures;
    JVal *v;
    if (!arg_only(args, (const char *[]){"failures", "value", NULL})) return NULL;
    if (!arg_i64(args, "failures", &failures)) return NULL;
    if (failures < 0 || failures > 10)
        return jifail("failures must be between 0 and 10");
    v = arg_req(args, "value");
    if (!v) return NULL;
    st->fail_calls++;
    if (st->fail_calls <= failures)
        return jfail("TRANSIENT_FAILURE", "transient failure (attempt %" PRId64 ")",
                     st->fail_calls);
    return v;
}

/* ------------------------------------------------------------------ */
/* operations table                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    OpFn fn;
} OpEntry;

static const OpEntry g_ops[] = {
    {"add", op_add},
    {"subtract", op_subtract},
    {"multiply", op_multiply},
    {"divide", op_divide},
    {"mod", op_mod},
    {"sum", op_sum},
    {"min", op_min},
    {"max", op_max},
    {"range", op_range},
    {"concat", op_concat},
    {"split", op_split},
    {"join", op_join},
    {"uppercase", op_uppercase},
    {"lowercase", op_lowercase},
    {"trim", op_trim},
    {"replace", op_replace},
    {"length", op_length},
    {"map", op_map},
    {"filter", op_filter},
    {"reduce", op_reduce},
    {"sort", op_sort},
    {"reverse", op_reverse},
    {"unique", op_unique},
    {"slice", op_slice},
    {"flatten", op_flatten},
    {"get", op_get},
    {"set", op_set},
    {"keys", op_keys},
    {"values", op_values},
    {"merge", op_merge},
    {"pick", op_pick},
    {"omit", op_omit},
    {"constant", op_constant},
    {"equals", op_equals},
    {"compare", op_compare},
    {"and", op_and},
    {"or", op_or},
    {"not", op_not},
    {"type", op_type},
    {"fail_until", op_fail_until},
    {NULL, NULL}
};

static OpFn find_op(const char *name, size_t len)
{
    const OpEntry *e;
    for (e = g_ops; e->name; e++) {
        if (strlen(e->name) == len && memcmp(e->name, name, len) == 0)
            return e->fn;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* structural validation                                               */
/* ------------------------------------------------------------------ */

static int is_id_start(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int is_id_char(unsigned char c)
{
    return is_id_start(c) || (c >= '0' && c <= '9');
}

static int valid_id(const char *s, size_t n)
{
    size_t i;
    if (n == 0) return 0;
    if (!is_id_start((unsigned char)s[0])) return 0;
    for (i = 1; i < n; i++)
        if (!is_id_char((unsigned char)s[i])) return 0;
    return 1;
}

static long find_step(Engine *e, const char *id, size_t idlen)
{
    size_t i;
    for (i = 0; i < e->n; i++) {
        if (e->steps[i].idlen == idlen &&
            memcmp(e->steps[i].id, id, idlen) == 0)
            return (long)i;
    }
    return -1;
}

static int only_keys_vals(JVal *o, const char **allowed, const char **what)
{
    size_t i;
    for (i = 0; i < o->on; i++) {
        int ok = 0;
        const char **a;
        for (a = allowed; *a; a++) {
            size_t alen = strlen(*a);
            if (o->klens[i] == alen && memcmp(o->keys[i], *a, alen) == 0) {
                ok = 1;
                break;
            }
        }
        if (!ok) {
            snprintf(g_errmsg, sizeof g_errmsg, "unknown %s field '%s'",
                     what ? *what : "object", o->keys[i]);
            return 0;
        }
    }
    return 1;
}

/* validates shape of a condition; returns 1 ok, 0 error (msg in g_errmsg) */
static int cond_shape_ok(JVal *c)
{
    static const char *cmps[] = {"eq", "ne", "gt", "gte", "lt", "lte", NULL};
    JVal *op;
    const char **a;
    int iscmp = 0;
    if (!c || c->t != J_OBJ) {
        snprintf(g_errmsg, sizeof g_errmsg, "condition must be an object");
        return 0;
    }
    op = jobj_get(c, "op");
    if (!op || op->t != J_STR) {
        snprintf(g_errmsg, sizeof g_errmsg, "condition requires a string op");
        return 0;
    }
    for (a = cmps; *a; a++)
        if (strcmp(op->s, *a) == 0) { iscmp = 1; break; }
    if (iscmp) {
        static const char *allowed[] = {"op", "left", "right", NULL};
        if (!only_keys_vals(c, allowed, (const char *[]){"condition", NULL}))
            return 0;
        if (!jobj_get(c, "left") || !jobj_get(c, "right")) {
            snprintf(g_errmsg, sizeof g_errmsg,
                     "comparison condition requires left and right");
            return 0;
        }
        return 1;
    }
    if (strcmp(op->s, "all") == 0 || strcmp(op->s, "any") == 0) {
        static const char *allowed[] = {"op", "conditions", NULL};
        JVal *cs;
        size_t i;
        if (!only_keys_vals(c, allowed, (const char *[]){"condition", NULL}))
            return 0;
        cs = jobj_get(c, "conditions");
        if (!cs || cs->t != J_ARR) {
            snprintf(g_errmsg, sizeof g_errmsg,
                     "'%s' condition requires a conditions array", op->s);
            return 0;
        }
        for (i = 0; i < cs->n; i++)
            if (!cond_shape_ok(cs->items[i])) return 0;
        return 1;
    }
    if (strcmp(op->s, "not") == 0) {
        static const char *allowed[] = {"op", "condition", NULL};
        JVal *inner;
        if (!only_keys_vals(c, allowed, (const char *[]){"condition", NULL}))
            return 0;
        inner = jobj_get(c, "condition");
        if (!inner) {
            snprintf(g_errmsg, sizeof g_errmsg,
                     "'not' condition requires a condition");
            return 0;
        }
        return cond_shape_ok(inner);
    }
    /* unknown condition op: evaluated at runtime -> INVALID_CONDITION */
    return 1;
}

static int validate(JVal *doc, Engine *eng)
{
    JVal *ver, *steps;
    size_t n, i, j;

    if (!doc || doc->t != J_OBJ) {
        verr("INVALID_WORKFLOW", NULL, "workflow must be a JSON object");
        return 0;
    }
    {
        static const char *allowed[] = {"version", "steps", NULL};
        if (!only_keys_vals(doc, allowed, (const char *[]){"top-level", NULL})) {
            verr("INVALID_WORKFLOW", NULL, "%s", g_errmsg);
            return 0;
        }
    }
    ver = jobj_get(doc, "version");
    if (!ver) {
        verr("INVALID_WORKFLOW", NULL, "missing required field 'version'");
        return 0;
    }
    if (!(ver->t == J_INT && ver->i == 1)) {
        verr("INVALID_WORKFLOW", NULL, "version must be the integer 1");
        return 0;
    }
    steps = jobj_get(doc, "steps");
    if (!steps) {
        verr("INVALID_WORKFLOW", NULL, "missing required field 'steps'");
        return 0;
    }
    if (steps->t != J_ARR) {
        verr("INVALID_WORKFLOW", NULL, "'steps' must be an array");
        return 0;
    }

    n = steps->n;
    eng->n = n;
    eng->steps = xcalloc(n ? n : 1, sizeof(Step));

    /* pass 1: per-step fields */
    for (i = 0; i < n; i++) {
        JVal *so = steps->items[i];
        Step *st = &eng->steps[i];
        JVal *idv, *opv, *args, *deps, *when, *retry;
        const char *idstr = NULL;

        memset(st, 0, sizeof *st);
        st->max_attempts = 1;
        if (!so || so->t != J_OBJ) {
            verr("INVALID_WORKFLOW", NULL, "step must be an object");
            return 0;
        }
        idv = jobj_get(so, "id");
        if (idv && idv->t == J_STR) idstr = idv->s;

        /* unknown step fields */
        {
            size_t k;
            static const char *allowed[] = {"id", "op", "args", "depends_on",
                                            "when", "retry", NULL};
            int bad = 0;
            for (k = 0; k < so->on && !bad; k++) {
                int ok = 0;
                const char **a;
                for (a = allowed; *a; a++)
                    if (strcmp(so->keys[k], *a) == 0) { ok = 1; break; }
                if (!ok) {
                    verr("INVALID_WORKFLOW", idstr,
                         "unknown step field '%s'", so->keys[k]);
                    return 0;
                }
            }
        }

        if (!idv || idv->t != J_STR) {
            verr("INVALID_WORKFLOW", NULL, "step 'id' must be a string");
            return 0;
        }
        if (!valid_id(idv->s, idv->slen)) {
            verr("INVALID_WORKFLOW", idstr, "invalid step id '%s'", idstr);
            return 0;
        }
        for (j = 0; j < i; j++) {
            if (eng->steps[j].idlen == idv->slen &&
                memcmp(eng->steps[j].id, idv->s, idv->slen) == 0) {
                verr("INVALID_WORKFLOW", idstr, "duplicate step id '%s'",
                     idstr);
                return 0;
            }
        }
        st->id = idv->s;
        st->idlen = idv->slen;

        opv = jobj_get(so, "op");
        if (!opv || opv->t != J_STR) {
            verr("INVALID_WORKFLOW", idstr, "step 'op' must be a string");
            return 0;
        }
        st->fn = find_op(opv->s, opv->slen);
        if (!st->fn) {
            verr("INVALID_WORKFLOW", idstr, "unknown operation '%s'", opv->s);
            return 0;
        }

        args = jobj_get(so, "args");
        if (args && args->t != J_OBJ) {
            verr("INVALID_WORKFLOW", idstr, "step 'args' must be an object");
            return 0;
        }
        st->args = args;

        deps = jobj_get(so, "depends_on");
        if (deps) {
            if (deps->t != J_ARR) {
                verr("INVALID_WORKFLOW", idstr,
                     "step 'depends_on' must be an array");
                return 0;
            }
            st->depnames = xmalloc((deps->n ? deps->n : 1) * sizeof *st->depnames);
            st->depnlens = xmalloc((deps->n ? deps->n : 1) * sizeof *st->depnlens);
            st->ndepnames = deps->n;
            for (j = 0; j < deps->n; j++) {
                if (deps->items[j]->t != J_STR) {
                    verr("INVALID_WORKFLOW", idstr,
                         "step 'depends_on' entries must be strings");
                    return 0;
                }
                st->depnames[j] = deps->items[j];
                st->depnlens[j] = deps->items[j]->slen;
            }
        }

        when = jobj_get(so, "when");
        if (when) {
            if (!cond_shape_ok(when)) {
                verr("INVALID_WORKFLOW", idstr, "%s", g_errmsg);
                return 0;
            }
            st->when = when;
        }

        retry = jobj_get(so, "retry");
        if (retry) {
            JVal *ma;
            static const char *allowed[] = {"max_attempts", NULL};
            if (retry->t != J_OBJ) {
                verr("INVALID_WORKFLOW", idstr, "step 'retry' must be an object");
                return 0;
            }
            if (!only_keys_vals(retry, allowed, (const char *[]){"retry", NULL})) {
                verr("INVALID_WORKFLOW", idstr, "%s", g_errmsg);
                return 0;
            }
            ma = jobj_get(retry, "max_attempts");
            if (!ma || ma->t != J_INT || ma->i < 1 || ma->i > 5) {
                verr("INVALID_WORKFLOW", idstr,
                     "retry.max_attempts must be an integer between 1 and 5");
                return 0;
            }
            st->max_attempts = (int)ma->i;
        }
    }

    /* pass 2: resolve dependencies */
    for (i = 0; i < n; i++) {
        Step *st = &eng->steps[i];
        size_t k = 0;
        st->depidx = xmalloc((st->ndepnames ? st->ndepnames : 1) *
                             sizeof *st->depidx);
        for (j = 0; j < st->ndepnames; j++) {
            long idx = find_step(eng, st->depnames[j]->s, st->depnlens[j]);
            size_t t;
            int dup = 0;
            if (idx < 0) {
                verr("UNKNOWN_DEPENDENCY", st->id,
                     "unknown dependency '%.*s'", (int)st->depnlens[j],
                     st->depnames[j]->s);
                return 0;
            }
            for (t = 0; t < k; t++)
                if (st->depidx[t] == (size_t)idx) { dup = 1; break; }
            if (!dup) st->depidx[k++] = (size_t)idx;
        }
        st->ndeps = k;
    }

    /* pass 3: cycle detection (Kahn) */
    {
        size_t *indeg = xcalloc(n ? n : 1, sizeof *indeg);
        size_t *outdeg = xcalloc(n ? n : 1, sizeof *outdeg);
        size_t *off, *fill, *adj, *queue, *topo;
        size_t edges = 0, head = 0, tail = 0, count = 0;

        for (i = 0; i < n; i++) {
            indeg[i] = eng->steps[i].ndeps;
            for (j = 0; j < eng->steps[i].ndeps; j++)
                outdeg[eng->steps[i].depidx[j]]++;
        }
        off = xcalloc(n + 1, sizeof *off);
        for (i = 0; i < n; i++) off[i + 1] = off[i] + outdeg[i];
        edges = off[n];
        adj = xmalloc((edges ? edges : 1) * sizeof *adj);
        fill = xmalloc((n ? n : 1) * sizeof *fill);
        memcpy(fill, off, n * sizeof *fill);
        for (i = 0; i < n; i++)
            for (j = 0; j < eng->steps[i].ndeps; j++) {
                size_t d = eng->steps[i].depidx[j];
                adj[fill[d]++] = i;
            }
        queue = xmalloc((n ? n : 1) * sizeof *queue);
        topo = xmalloc((n ? n : 1) * sizeof *topo);
        for (i = 0; i < n; i++)
            if (indeg[i] == 0) queue[tail++] = i;
        while (head < tail) {
            size_t u = queue[head++];
            size_t k;
            topo[count++] = u;
            for (k = off[u]; k < off[u + 1]; k++) {
                size_t v = adj[k];
                if (indeg[v] > 0 && --indeg[v] == 0) queue[tail++] = v;
            }
        }
        if (count < n) {
            verr("CYCLIC_DEPENDENCY", NULL, "workflow contains a dependency cycle");
            return 0;
        }
        /* transitive closure in topological order */
        eng->clo = xcalloc(n ? n * n : 1, 1);
        for (i = 0; i < n; i++) {
            size_t u = topo[i];
            size_t m;
            for (j = 0; j < eng->steps[u].ndeps; j++) {
                size_t d = eng->steps[u].depidx[j];
                eng->clo[u * n + d] = 1;
                for (m = 0; m < n; m++)
                    if (eng->clo[d * n + m]) eng->clo[u * n + m] = 1;
            }
        }
        free(indeg);
        free(outdeg);
        free(off);
        free(fill);
        free(adj);
        free(queue);
        free(topo);
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* reference resolution                                                */
/* ------------------------------------------------------------------ */

static int is_ref(JVal *v)
{
    if (!v || v->t != J_OBJ) return 0;
    if (v->on != 1) return 0;
    if (v->klens[0] != 4 || memcmp(v->keys[0], "$ref", 4) != 0) return 0;
    return v->vals[0]->t == J_STR;
}

static JVal *resolve_ref(Ctx *c, size_t si, JVal *node)
{
    Engine *e = c->eng;
    size_t n = e->n;
    JVal *refv = node->vals[0];
    const char *s = refv->s;
    size_t len = refv->slen;
    size_t dot = 0, pos;
    long idx;
    JVal *cur;

    while (dot < len && s[dot] != '.') dot++;
    if (dot == 0)
        return jfail("INVALID_REFERENCE", "empty step id in reference");
    idx = find_step(e, s, dot);
    if (idx < 0)
        return jfail("INVALID_REFERENCE", "reference to unknown step '%.*s'",
                     (int)dot, s);
    if ((size_t)idx == si || !e->clo[si * n + (size_t)idx])
        return jfail("INVALID_REFERENCE",
                     "step '%.*s' is not a dependency of the referencing step",
                     (int)dot, s);
    if (c->skipped[(size_t)idx])
        return jfail("SKIPPED_REFERENCE",
                     "reference to skipped step '%.*s'", (int)dot, s);
    cur = c->res[(size_t)idx];
    pos = dot;
    while (pos < len) {
        size_t seg;
        if (s[pos] != '.')
            return jfail("INVALID_REFERENCE", "malformed reference path");
        pos++;
        seg = pos;
        while (pos < len && s[pos] != '.') pos++;
        if (pos == seg)
            return jfail("INVALID_REFERENCE", "empty path segment in reference");
        if (cur->t == J_OBJ) {
            JVal *nx = jobj_getn(cur, s + seg, pos - seg);
            if (!nx)
                return jfail("INVALID_REFERENCE", "missing key '%.*s' in reference",
                             (int)(pos - seg), s + seg);
            cur = nx;
        } else if (cur->t == J_ARR) {
            size_t k;
            uint64_t ix = 0;
            int all_digits = 1;
            for (k = seg; k < pos; k++) {
                unsigned dig;
                if (s[k] < '0' || s[k] > '9') { all_digits = 0; break; }
                dig = (unsigned)(s[k] - '0');
                if (ix > (UINT64_MAX - (uint64_t)dig) / 10)
                    return jfail("INVALID_REFERENCE",
                                 "array index out of range");
                ix = ix * 10 + (uint64_t)dig;
            }
            if (!all_digits)
                return jfail("INVALID_REFERENCE",
                             "array index '%.*s' is not a non-negative integer",
                             (int)(pos - seg), s + seg);
            if (ix >= cur->n)
                return jfail("INVALID_REFERENCE",
                             "array index %" PRIu64 " out of range", ix);
            cur = cur->items[(size_t)ix];
        } else {
            return jfail("INVALID_REFERENCE",
                         "reference path traverses a non-container value");
        }
    }
    return cur;
}

static JVal *resolve_rec(Ctx *c, size_t si, JVal *v, int depth)
{
    size_t i;
    if (depth > 256)
        return jfail("INVALID_REFERENCE", "reference nesting too deep");
    if (is_ref(v)) return resolve_ref(c, si, v);
    if (v->t == J_ARR) {
        for (i = 0; i < v->n; i++) {
            JVal *r = resolve_rec(c, si, v->items[i], depth + 1);
            if (!r) return NULL;
            v->items[i] = r;
        }
        return v;
    }
    if (v->t == J_OBJ) {
        for (i = 0; i < v->on; i++) {
            JVal *r = resolve_rec(c, si, v->vals[i], depth + 1);
            if (!r) return NULL;
            v->vals[i] = r;
        }
        return v;
    }
    return v;
}

/* ------------------------------------------------------------------ */
/* conditions                                                          */
/* ------------------------------------------------------------------ */

static int eval_cond(Ctx *c, size_t si, JVal *cond, JVal **out);

static int eval_cond_cmp(Ctx *c, size_t si, JVal *cond, JVal **out)
{
    static const char *names[] = {"eq", "ne", "gt", "gte", "lt", "lte", NULL};
    JVal *op = jobj_get(cond, "op");
    JVal *left, *right;
    int which = 0, i;
    for (i = 0; names[i]; i++)
        if (strcmp(op->s, names[i]) == 0) { which = i; break; }
    left = resolve_rec(c, si, jobj_get(cond, "left"), 0);
    if (!left) return 0;
    right = resolve_rec(c, si, jobj_get(cond, "right"), 0);
    if (!right) return 0;
    switch (which) {
    case 0: *out = jbool(valeq(left, right)); break;
    case 1: *out = jbool(!valeq(left, right)); break;
    case 2: *out = jbool(valcmp(left, right) > 0); break;
    case 3: *out = jbool(valcmp(left, right) >= 0); break;
    case 4: *out = jbool(valcmp(left, right) < 0); break;
    default: *out = jbool(valcmp(left, right) <= 0); break;
    }
    return 1;
}

static int eval_cond(Ctx *c, size_t si, JVal *cond, JVal **out)
{
    JVal *op;
    static const char *cmps[] = {"eq", "ne", "gt", "gte", "lt", "lte", NULL};
    int iscmp = 0, i;
    if (!cond || cond->t != J_OBJ) {
        seterr("INVALID_CONDITION", "condition is not an object");
        return 0;
    }
    op = jobj_get(cond, "op");
    if (!op || op->t != J_STR) {
        seterr("INVALID_CONDITION", "condition has no string op");
        return 0;
    }
    for (i = 0; cmps[i]; i++)
        if (strcmp(op->s, cmps[i]) == 0) { iscmp = 1; break; }
    if (iscmp) return eval_cond_cmp(c, si, cond, out);
    if (strcmp(op->s, "all") == 0 || strcmp(op->s, "any") == 0) {
        JVal *cs = jobj_get(cond, "conditions");
        int is_all = strcmp(op->s, "all") == 0;
        size_t k;
        if (!cs || cs->t != J_ARR) {
            seterr("INVALID_CONDITION", "'%s' requires a conditions array", op->s);
            return 0;
        }
        for (k = 0; k < cs->n; k++) {
            JVal *r = NULL;
            if (!eval_cond(c, si, cs->items[k], &r)) return 0;
            if (!r || r->t != J_BOOL) {
                seterr("INVALID_CONDITION", "condition did not yield a boolean");
                return 0;
            }
            if (is_all && !r->b) { *out = jbool(0); return 1; }
            if (!is_all && r->b) { *out = jbool(1); return 1; }
        }
        *out = jbool(is_all);
        return 1;
    }
    if (strcmp(op->s, "not") == 0) {
        JVal *inner = jobj_get(cond, "condition");
        JVal *r = NULL;
        if (!inner) {
            seterr("INVALID_CONDITION", "'not' requires a condition");
            return 0;
        }
        if (!eval_cond(c, si, inner, &r)) return 0;
        if (!r || r->t != J_BOOL) {
            seterr("INVALID_CONDITION", "condition did not yield a boolean");
            return 0;
        }
        *out = jbool(!r->b);
        return 1;
    }
    seterr("INVALID_CONDITION", "unknown condition operator '%s'", op->s);
    return 0;
}

/* ------------------------------------------------------------------ */
/* response emission                                                   */
/* ------------------------------------------------------------------ */

static void emit_response(const char *status, JVal *results, JVal *order,
                          JVal *skippedlist, const char *code, const char *step,
                          const char *msg)
{
    Buf b;
    buf_init(&b, 512);
    buf_puts(&b, "{\"status\":\"");
    buf_puts(&b, status);
    buf_puts(&b, "\",\"results\":");
    ser(&b, results);
    buf_puts(&b, ",\"execution_order\":");
    ser(&b, order);
    buf_puts(&b, ",\"skipped\":");
    ser(&b, skippedlist);
    buf_puts(&b, ",\"error\":");
    if (!code) {
        buf_puts(&b, "null");
    } else {
        buf_puts(&b, "{\"code\":\"");
        json_escape(&b, code, strlen(code));
        buf_puts(&b, "\",\"step\":");
        if (step) {
            buf_putc(&b, '"');
            json_escape(&b, step, strlen(step));
            buf_putc(&b, '"');
        } else {
            buf_puts(&b, "null");
        }
        buf_puts(&b, ",\"message\":\"");
        json_escape(&b, msg ? msg : "", strlen(msg ? msg : ""));
        buf_puts(&b, "\"}");
    }
    buf_puts(&b, "}\n");
    fwrite(b.p, 1, b.n, stdout);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* execution                                                           */
/* ------------------------------------------------------------------ */

static int run_engine(Engine *eng)
{
    size_t n = eng->n;
    Ctx ctx;
    JVal *results = jobj();
    JVal *order = jarr();
    JVal *skiparr = jarr();
    JVal **res = xcalloc(n ? n : 1, sizeof *res);
    unsigned char *finished = xcalloc(n ? n : 1, 1);
    unsigned char *skipped = xcalloc(n ? n : 1, 1);
    size_t done = 0;
    char fail_code[64] = {0};
    char fail_msg[1024] = {0};
    size_t fail_step = (size_t)-1;
    int internal = 0;

    ctx.eng = eng;
    ctx.res = res;
    ctx.finished = finished;
    ctx.skipped = skipped;

    while (done < n) {
        long pick = -1;
        Step *st;
        JVal *val = NULL;
        JVal *args;
        int attempts = 0;
        size_t i, j;

        for (i = 0; i < n; i++) {
            int ready = 1;
            if (finished[i]) continue;
            for (j = 0; j < eng->steps[i].ndeps; j++) {
                if (!finished[eng->steps[i].depidx[j]]) { ready = 0; break; }
            }
            if (ready) { pick = (long)i; break; }
        }
        if (pick < 0) {
            snprintf(fail_code, sizeof fail_code, "INTERNAL_ERROR");
            snprintf(fail_msg, sizeof fail_msg,
                     "no runnable step found (internal scheduling error)");
            internal = 1;
            break;
        }
        st = &eng->steps[pick];

        if (st->when) {
            JVal *cv = NULL;
            if (!eval_cond(&ctx, (size_t)pick, st->when, &cv)) {
                snprintf(fail_code, sizeof fail_code, "%s",
                         g_errcode ? g_errcode : "INVALID_CONDITION");
                snprintf(fail_msg, sizeof fail_msg, "%.500s", g_errmsg);
                fail_step = (size_t)pick;
                break;
            }
            if (!cv || cv->t != J_BOOL) {
                snprintf(fail_code, sizeof fail_code, "INVALID_CONDITION");
                snprintf(fail_msg, sizeof fail_msg,
                         "when condition did not evaluate to a boolean");
                fail_step = (size_t)pick;
                break;
            }
            if (!cv->b) {
                JVal *idv = jstrn(st->id, st->idlen);
                res[pick] = jnull();
                skipped[pick] = 1;
                finished[pick] = 1;
                done++;
                jarr_push(order, idv);
                jarr_push(skiparr, jstrn(st->id, st->idlen));
                jobj_setn(results, st->id, st->idlen, jnull());
                continue;
            }
        }

        if (st->args) {
            args = resolve_rec(&ctx, (size_t)pick, st->args, 0);
            if (!args) {
                snprintf(fail_code, sizeof fail_code, "%s",
                         g_errcode ? g_errcode : "INVALID_REFERENCE");
                snprintf(fail_msg, sizeof fail_msg, "%.500s", g_errmsg);
                fail_step = (size_t)pick;
                break;
            }
        } else {
            args = jobj();
        }

        for (;;) {
            attempts++;
            g_errcode = NULL;
            g_errmsg[0] = 0;
            val = st->fn(st, args);
            if (val) break;
            {
                const char *code = g_errcode ? g_errcode : "INTERNAL_ERROR";
                if (strcmp(code, "TRANSIENT_FAILURE") == 0 &&
                    attempts < st->max_attempts)
                    continue;
                if (strcmp(code, "TRANSIENT_FAILURE") == 0) {
                    snprintf(fail_code, sizeof fail_code, "STEP_FAILED");
                    snprintf(fail_msg, sizeof fail_msg,
                             "step '%s' failed after %d attempt(s): %.500s",
                             st->id, attempts, g_errmsg);
                } else {
                    snprintf(fail_code, sizeof fail_code, "%s", code);
                    snprintf(fail_msg, sizeof fail_msg,
                             "step '%s' failed: %.500s", st->id, g_errmsg);
                    if (strcmp(code, "INTERNAL_ERROR") == 0) internal = 1;
                }
                fail_step = (size_t)pick;
                break;
            }
        }
        if (!val) break;

        res[pick] = val;
        finished[pick] = 1;
        done++;
        jarr_push(order, jstrn(st->id, st->idlen));
        jobj_setn(results, st->id, st->idlen, val);
    }

    if (fail_code[0]) {
        emit_response("failed", results, order, skiparr, fail_code,
                      fail_step != (size_t)-1 ? eng->steps[fail_step].id : NULL,
                      fail_msg);
        return internal ? 3 : 1;
    }
    emit_response("completed", results, order, skiparr, NULL, NULL, NULL);
    return 0;
}

/* ------------------------------------------------------------------ */
/* input                                                               */
/* ------------------------------------------------------------------ */

static char *read_all(FILE *f, size_t *out_len)
{
    Buf b;
    char tmp[65536];
    size_t got;
    buf_init(&b, 4096);
    while ((got = fread(tmp, 1, sizeof tmp, f)) > 0)
        buf_put(&b, tmp, got);
    *out_len = b.n;
    return b.p;
}

int main(int argc, char **argv)
{
    char *text;
    size_t len = 0;
    P ps;
    JVal *doc;
    Engine eng;
    int rc;

    memset(&eng, 0, sizeof eng);

    if (argc != 3 || strcmp(argv[1], "run") != 0) {
        emit_response("failed", jobj(), jarr(), jarr(), "INVALID_WORKFLOW", NULL,
                      "usage: workflow run <workflow.json> (or '-' for stdin)");
        return 2;
    }

    if (strcmp(argv[2], "-") == 0) {
        text = read_all(stdin, &len);
    } else {
        FILE *f = fopen(argv[2], "rb");
        if (!f) {
            emit_response("failed", jobj(), jarr(), jarr(), "INVALID_WORKFLOW",
                          NULL, "cannot open input file");
            return 2;
        }
        text = read_all(f, &len);
        fclose(f);
    }

    ps.p = text;
    ps.end = text + len;
    ps.depth = 0;
    ps.msg[0] = 0;
    doc = parse_value(&ps);
    if (doc) {
        pskip(&ps);
        if (ps.p != ps.end) {
            perr(&ps, "trailing characters after JSON document");
            doc = NULL;
        }
    }
    if (!doc) {
        emit_response("failed", jobj(), jarr(), jarr(), "INVALID_WORKFLOW", NULL,
                      ps.msg[0] ? ps.msg : "invalid JSON input");
        return 2;
    }

    if (!validate(doc, &eng)) {
        emit_response("failed", jobj(), jarr(), jarr(),
                      g_errcode ? g_errcode : "INVALID_WORKFLOW", g_errstep,
                      g_errmsg);
        return 2;
    }

    rc = run_engine(&eng);
    return rc;
}
