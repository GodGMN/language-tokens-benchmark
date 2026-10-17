/*
 * workflow.c -- declarative workflow engine CLI (C11)
 *
 * Implements the contract in SPEC.md:
 *   workflow run <workflow.json>
 *   workflow run -
 *
 * Only the C standard library is used (no external dependencies).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <inttypes.h>
#include <errno.h>
#include <limits.h>

/* ------------------------------------------------------------------ */
/* Small utilities                                                     */
/* ------------------------------------------------------------------ */

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "workflow: out of memory\n");
        exit(3);
    }
    return p;
}

static void *xcalloc(size_t n, size_t s) {
    void *p = calloc(n ? n : 1, s ? s : 1);
    if (!p) {
        fprintf(stderr, "workflow: out of memory\n");
        exit(3);
    }
    return p;
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fprintf(stderr, "workflow: out of memory\n");
        exit(3);
    }
    return q;
}

static char *xstrndup(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/* Growable byte buffer. */
typedef struct {
    char  *p;
    size_t n;
    size_t cap;
} Buf;

static void bgrow(Buf *b, size_t need) {
    if (b->n + need <= b->cap) return;
    size_t nc = b->cap ? b->cap : 128;
    while (nc < b->n + need) nc *= 2;
    b->p = xrealloc(b->p, nc);
    b->cap = nc;
}

static void bput(Buf *b, const void *s, size_t n) {
    if (!n) return;
    bgrow(b, n);
    memcpy(b->p + b->n, s, n);
    b->n += n;
}

static void bputc(Buf *b, char c) {
    bgrow(b, 1);
    b->p[b->n++] = c;
}

static void bputs(Buf *b, const char *s) {
    bput(b, s, strlen(s));
}

/* ------------------------------------------------------------------ */
/* JSON value representation                                           */
/* ------------------------------------------------------------------ */

typedef enum {
    J_NULL = 0,
    J_BOOL = 1,
    J_NUM  = 2,
    J_STR  = 3,
    J_ARR  = 4,
    J_OBJ  = 5
} JType;

typedef struct JVal JVal;

typedef struct {
    char  *key;
    size_t klen;
    JVal  *val;
} JMem;

struct JVal {
    JType t;

    int b;              /* J_BOOL */

    int       is_int;   /* number literal without fraction/exponent, or computed int */
    int       fits;     /* integer fits in i64 */
    int64_t   i;        /* integer value if fits */
    long double d;      /* numeric value (used for equality/ordering) */
    char     *raw;      /* original literal text (non-integer numbers) */

    char   *s;          /* J_STR: NUL terminated too */
    size_t  slen;

    JVal  **it;         /* J_ARR */
    size_t  n, cap;

    JMem   *mem;        /* J_OBJ, insertion order, may contain dup keys if parsed */
    size_t  mn, mcap;
};

static JVal *jnew(JType t) {
    JVal *v = xcalloc(1, sizeof(JVal));
    v->t = t;
    return v;
}

static JVal *jnull(void) {
    static JVal *n = NULL;
    if (!n) n = jnew(J_NULL);
    return n;
}

static JVal *jbool(int b) {
    JVal *v = jnew(J_BOOL);
    v->b = b ? 1 : 0;
    return v;
}

static JVal *jint(int64_t i) {
    JVal *v = jnew(J_NUM);
    v->is_int = 1;
    v->fits = 1;
    v->i = i;
    v->d = (long double)i;
    return v;
}

static JVal *jstr_take(char *p, size_t n) {
    JVal *v = jnew(J_STR);
    v->s = p;
    v->slen = n;
    return v;
}

static JVal *jstr(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return jstr_take(p, n);
}

static JVal *jarr(void) {
    return jnew(J_ARR);
}

static JVal *jobj(void) {
    return jnew(J_OBJ);
}

static void a_push(JVal *a, JVal *v) {
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 8;
        a->it = xrealloc(a->it, a->cap * sizeof(JVal *));
    }
    a->it[a->n++] = v;
}

/* Append, preserving duplicate keys (used by the parser). */
static void o_append(JVal *o, const char *k, size_t klen, JVal *v) {
    if (o->mn == o->mcap) {
        o->mcap = o->mcap ? o->mcap * 2 : 8;
        o->mem = xrealloc(o->mem, o->mcap * sizeof(JMem));
    }
    o->mem[o->mn].key  = xstrndup(k, klen);
    o->mem[o->mn].klen = klen;
    o->mem[o->mn].val  = v;
    o->mn++;
}

/* Set (replace if already present, else append) -- used to build results. */
static void o_put(JVal *o, const char *k, size_t klen, JVal *v) {
    for (size_t i = 0; i < o->mn; i++) {
        if (o->mem[i].klen == klen && memcmp(o->mem[i].key, k, klen) == 0) {
            o->mem[i].val = v;
            return;
        }
    }
    o_append(o, k, klen, v);
}

/* First occurrence lookup (duplicate keys: first wins). */
static JVal *o_get(const JVal *o, const char *name) {
    size_t n = strlen(name);
    if (!o || o->t != J_OBJ) return NULL;
    for (size_t i = 0; i < o->mn; i++) {
        if (o->mem[i].klen == n && memcmp(o->mem[i].key, name, n) == 0)
            return o->mem[i].val;
    }
    return NULL;
}

static int is_i64(const JVal *v) {
    return v && v->t == J_NUM && v->is_int && v->fits;
}

/* ------------------------------------------------------------------ */
/* JSON parser                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *p;
    const char *end;
    int         depth;
    int         failed;
    char        err[160];
} Parser;

static void skip_ws(Parser *ps) {
    while (ps->p < ps->end) {
        char c = *ps->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ps->p++;
        else break;
    }
}

static JVal *perrf(Parser *ps, const char *fmt, ...) {
    if (!ps->failed) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(ps->err, sizeof ps->err, fmt, ap);
        va_end(ap);
        ps->failed = 1;
    }
    return NULL;
}

static void utf8_encode(Buf *b, uint32_t cp) {
    char t[4];
    size_t n;
    if (cp < 0x80) {
        t[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        t[0] = (char)(0xC0 | (cp >> 6));
        t[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        t[0] = (char)(0xE0 | (cp >> 12));
        t[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        t[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        t[0] = (char)(0xF0 | (cp >> 18));
        t[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        t[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        t[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    bput(b, t, n);
}

static int hex4(Parser *ps, uint32_t *out) {
    if (ps->end - ps->p < 4) return -1;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        char c = ps->p[i];
        uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else return -1;
        v = (v << 4) | d;
    }
    ps->p += 4;
    *out = v;
    return 0;
}

/* Parses a string; current char must be '"'. */
static int p_string(Parser *ps, char **out, size_t *outlen) {
    Buf b;
    memset(&b, 0, sizeof b);
    ps->p++; /* opening quote */
    for (;;) {
        if (ps->p >= ps->end) {
            free(b.p);
            perrf(ps, "unterminated string");
            return -1;
        }
        unsigned char c = (unsigned char)*ps->p;
        if (c == '"') {
            ps->p++;
            break;
        }
        if (c == '\\') {
            ps->p++;
            if (ps->p >= ps->end) {
                free(b.p);
                perrf(ps, "bad escape");
                return -1;
            }
            char e = *ps->p++;
            switch (e) {
            case '"':  bputc(&b, '"');  break;
            case '\\': bputc(&b, '\\'); break;
            case '/':  bputc(&b, '/');  break;
            case 'b':  bputc(&b, '\b'); break;
            case 'f':  bputc(&b, '\f'); break;
            case 'n':  bputc(&b, '\n'); break;
            case 'r':  bputc(&b, '\r'); break;
            case 't':  bputc(&b, '\t'); break;
            case 'u': {
                uint32_t cp;
                if (hex4(ps, &cp) != 0) {
                    free(b.p);
                    perrf(ps, "bad \\u escape");
                    return -1;
                }
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                        const char *save = ps->p;
                        ps->p += 2;
                        uint32_t lo;
                        if (hex4(ps, &lo) != 0) {
                            free(b.p);
                            perrf(ps, "bad \\u escape");
                            return -1;
                        }
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                        } else {
                            ps->p = save;
                            cp = 0xFFFD;
                        }
                    } else {
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;
                }
                utf8_encode(&b, cp);
                break;
            }
            default:
                free(b.p);
                perrf(ps, "bad escape '\\%c'", e);
                return -1;
            }
        } else if (c < 0x20) {
            free(b.p);
            perrf(ps, "control character in string");
            return -1;
        } else {
            bputc(&b, (char)c);
            ps->p++;
        }
    }
    bputc(&b, '\0');
    *out = b.p;
    *outlen = b.n - 1;
    return 0;
}

static JVal *jnum_lit(const char *s, size_t len, int is_int) {
    JVal *v = jnew(J_NUM);
    v->is_int = is_int;
    v->raw = xstrndup(s, len);
    char *z = xstrndup(s, len);
    char *endp = NULL;
    errno = 0;
    long double d = strtold(z, &endp);
    v->d = d;
    if (is_int) {
        errno = 0;
        long long iv = strtoll(z, &endp, 10);
        if (errno == ERANGE) {
            v->fits = 0;
        } else {
            v->fits = 1;
            v->i = (int64_t)iv;
        }
    }
    free(z);
    return v;
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static JVal *p_number(Parser *ps) {
    const char *st = ps->p;
    const char *q = ps->p;
    int is_int = 1;

    if (q < ps->end && *q == '-') q++;
    if (q >= ps->end) return perrf(ps, "bad number");
    if (*q == '0') {
        q++;
    } else if (*q >= '1' && *q <= '9') {
        while (q < ps->end && is_digit(*q)) q++;
    } else {
        return perrf(ps, "bad number");
    }
    if (q < ps->end && *q == '.') {
        is_int = 0;
        q++;
        if (q >= ps->end || !is_digit(*q)) return perrf(ps, "bad number");
        while (q < ps->end && is_digit(*q)) q++;
    }
    if (q < ps->end && (*q == 'e' || *q == 'E')) {
        is_int = 0;
        q++;
        if (q < ps->end && (*q == '+' || *q == '-')) q++;
        if (q >= ps->end || !is_digit(*q)) return perrf(ps, "bad number");
        while (q < ps->end && is_digit(*q)) q++;
    }
    JVal *v = jnum_lit(st, (size_t)(q - st), is_int);
    ps->p = q;
    return v;
}

static JVal *p_value(Parser *ps);

static JVal *p_array(Parser *ps) {
    if (++ps->depth > 400) return perrf(ps, "nesting too deep");
    ps->p++; /* '[' */
    JVal *a = jarr();
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == ']') {
        ps->p++;
        ps->depth--;
        return a;
    }
    for (;;) {
        JVal *v = p_value(ps);
        if (!v) return NULL;
        a_push(a, v);
        skip_ws(ps);
        if (ps->p >= ps->end) return perrf(ps, "unterminated array");
        if (*ps->p == ',') {
            ps->p++;
            continue;
        }
        if (*ps->p == ']') {
            ps->p++;
            ps->depth--;
            return a;
        }
        return perrf(ps, "expected ',' or ']'");
    }
}

static JVal *p_object(Parser *ps) {
    if (++ps->depth > 400) return perrf(ps, "nesting too deep");
    ps->p++; /* '{' */
    JVal *o = jobj();
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == '}') {
        ps->p++;
        ps->depth--;
        return o;
    }
    for (;;) {
        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != '"') return perrf(ps, "expected object key");
        char *k;
        size_t kl;
        if (p_string(ps, &k, &kl) != 0) return NULL;
        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != ':') {
            free(k);
            return perrf(ps, "expected ':'");
        }
        ps->p++;
        JVal *v = p_value(ps);
        if (!v) {
            free(k);
            return NULL;
        }
        o_append(o, k, kl, v);
        free(k);
        skip_ws(ps);
        if (ps->p >= ps->end) return perrf(ps, "unterminated object");
        if (*ps->p == ',') {
            ps->p++;
            continue;
        }
        if (*ps->p == '}') {
            ps->p++;
            ps->depth--;
            return o;
        }
        return perrf(ps, "expected ',' or '}'");
    }
}

static JVal *p_value(Parser *ps) {
    skip_ws(ps);
    if (ps->p >= ps->end) return perrf(ps, "unexpected end of input");
    char c = *ps->p;
    switch (c) {
    case '{':
        return p_object(ps);
    case '[':
        return p_array(ps);
    case '"': {
        char *s;
        size_t n;
        if (p_string(ps, &s, &n) != 0) return NULL;
        return jstr_take(s, n);
    }
    case 't':
        if (ps->end - ps->p >= 4 && memcmp(ps->p, "true", 4) == 0) {
            ps->p += 4;
            return jbool(1);
        }
        return perrf(ps, "bad literal");
    case 'f':
        if (ps->end - ps->p >= 5 && memcmp(ps->p, "false", 5) == 0) {
            ps->p += 5;
            return jbool(0);
        }
        return perrf(ps, "bad literal");
    case 'n':
        if (ps->end - ps->p >= 4 && memcmp(ps->p, "null", 4) == 0) {
            ps->p += 4;
            return jnull();
        }
        return perrf(ps, "bad literal");
    default:
        if (c == '-' || is_digit(c)) return p_number(ps);
        return perrf(ps, "unexpected character '%c'", c);
    }
}

/* ------------------------------------------------------------------ */
/* JSON writer                                                         */
/* ------------------------------------------------------------------ */

static void wstr(Buf *b, const char *s, size_t n) {
    bputc(b, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  bput(b, "\\\"", 2); break;
        case '\\': bput(b, "\\\\", 2); break;
        case '\n': bput(b, "\\n", 2);  break;
        case '\r': bput(b, "\\r", 2);  break;
        case '\t': bput(b, "\\t", 2);  break;
        case '\b': bput(b, "\\b", 2);  break;
        case '\f': bput(b, "\\f", 2);  break;
        default:
            if (c < 0x20) {
                char t[8];
                snprintf(t, sizeof t, "\\u%04x", (unsigned)c);
                bput(b, t, 6);
            } else {
                bputc(b, (char)c);
            }
        }
    }
    bputc(b, '"');
}

static void wnum(Buf *b, const JVal *v) {
    if (v->is_int && v->fits) {
        char t[32];
        int n = snprintf(t, sizeof t, "%" PRId64, v->i);
        bput(b, t, (size_t)n);
    } else if (v->raw) {
        bputs(b, v->raw);
    } else {
        char t[64];
        int n = snprintf(t, sizeof t, "%.17Lg", v->d);
        bput(b, t, (size_t)n);
    }
}

static void wjson(Buf *b, const JVal *v);

static void wjson(Buf *b, const JVal *v) {
    switch (v->t) {
    case J_NULL:
        bputs(b, "null");
        break;
    case J_BOOL:
        bputs(b, v->b ? "true" : "false");
        break;
    case J_NUM:
        wnum(b, v);
        break;
    case J_STR:
        wstr(b, v->s, v->slen);
        break;
    case J_ARR:
        bputc(b, '[');
        for (size_t i = 0; i < v->n; i++) {
            if (i) bputc(b, ',');
            wjson(b, v->it[i]);
        }
        bputc(b, ']');
        break;
    case J_OBJ:
        bputc(b, '{');
        for (size_t i = 0; i < v->mn; i++) {
            if (i) bputc(b, ',');
            wstr(b, v->mem[i].key, v->mem[i].klen);
            bputc(b, ':');
            wjson(b, v->mem[i].val);
        }
        bputc(b, '}');
        break;
    }
}

static JVal *jstr_from_buf(Buf *b) {
    bputc(b, '\0');
    return jstr_take(b->p, b->n - 1);
}

/* ------------------------------------------------------------------ */
/* Type names, truthiness, equality, total ordering                    */
/* ------------------------------------------------------------------ */

static const char *type_name(const JVal *v) {
    switch (v->t) {
    case J_NULL:  return "null";
    case J_BOOL:  return "boolean";
    case J_NUM:   return "number";
    case J_STR:   return "string";
    case J_ARR:   return "array";
    case J_OBJ:   return "object";
    }
    return "null";
}

static int truthy(const JVal *v) {
    switch (v->t) {
    case J_NULL:  return 0;
    case J_BOOL:  return v->b != 0;
    case J_NUM:   return v->d != 0;
    case J_STR:   return v->slen != 0;
    case J_ARR:   return v->n != 0;
    case J_OBJ:   return v->mn != 0;
    }
    return 0;
}

static int cmp_bytes(const char *a, size_t al, const char *b, size_t bl) {
    size_t m = al < bl ? al : bl;
    int c = m ? memcmp(a, b, m) : 0;
    if (c) return c < 0 ? -1 : 1;
    if (al < bl) return -1;
    if (al > bl) return 1;
    return 0;
}

static int jequal(const JVal *a, const JVal *b) {
    if (a->t != b->t) return 0;
    switch (a->t) {
    case J_NULL:
        return 1;
    case J_BOOL:
        return a->b == b->b;
    case J_NUM:
        if (a->is_int && a->fits && b->is_int && b->fits)
            return a->i == b->i;
        return a->d == b->d;
    case J_STR:
        return a->slen == b->slen && memcmp(a->s, b->s, a->slen) == 0;
    case J_ARR:
        if (a->n != b->n) return 0;
        for (size_t i = 0; i < a->n; i++)
            if (!jequal(a->it[i], b->it[i])) return 0;
        return 1;
    case J_OBJ:
        if (a->mn != b->mn) return 0;
        for (size_t i = 0; i < a->mn; i++) {
            JVal *bv = NULL;
            for (size_t j = 0; j < b->mn; j++) {
                if (b->mem[j].klen == a->mem[i].klen &&
                    memcmp(b->mem[j].key, a->mem[i].key, a->mem[i].klen) == 0) {
                    bv = b->mem[j].val;
                    break;
                }
            }
            if (!bv) return 0;
            if (!jequal(a->mem[i].val, bv)) return 0;
        }
        return 1;
    }
    return 0;
}

static int type_rank(const JVal *v) {
    switch (v->t) {
    case J_NUM:   return 0;
    case J_STR:   return 1;
    case J_BOOL:  return 2;
    case J_NULL:  return 3;
    case J_ARR:   return 4;
    case J_OBJ:   return 5;
    }
    return 6;
}

/* Stable insertion sort of member indices by key bytes. */
static void sort_member_idx(const JVal *o, size_t *idx) {
    for (size_t i = 1; i < o->mn; i++) {
        size_t key = idx[i];
        size_t j = i;
        while (j > 0) {
            JMem *p = &o->mem[idx[j - 1]];
            JMem *q = &o->mem[key];
            if (cmp_bytes(p->key, p->klen, q->key, q->klen) <= 0) break;
            idx[j] = idx[j - 1];
            j--;
        }
        idx[j] = key;
    }
}

static int compare_values(const JVal *a, const JVal *b) {
    int ra = type_rank(a), rb = type_rank(b);
    if (ra != rb) return ra < rb ? -1 : 1;
    switch (a->t) {
    case J_NULL:
        return 0;
    case J_BOOL:
        if (a->b == b->b) return 0;
        return a->b < b->b ? -1 : 1;
    case J_NUM:
        if (a->is_int && a->fits && b->is_int && b->fits) {
            if (a->i == b->i) return 0;
            return a->i < b->i ? -1 : 1;
        }
        if (a->d == b->d) return 0;
        return a->d < b->d ? -1 : 1;
    case J_STR:
        return cmp_bytes(a->s, a->slen, b->s, b->slen);
    case J_ARR: {
        size_t m = a->n < b->n ? a->n : b->n;
        for (size_t i = 0; i < m; i++) {
            int c = compare_values(a->it[i], b->it[i]);
            if (c) return c;
        }
        if (a->n == b->n) return 0;
        return a->n < b->n ? -1 : 1;
    }
    case J_OBJ: {
        size_t *ia = xmalloc((a->mn ? a->mn : 1) * sizeof(size_t));
        size_t *ib = xmalloc((b->mn ? b->mn : 1) * sizeof(size_t));
        for (size_t i = 0; i < a->mn; i++) ia[i] = i;
        for (size_t i = 0; i < b->mn; i++) ib[i] = i;
        sort_member_idx(a, ia);
        sort_member_idx(b, ib);
        size_t m = a->mn < b->mn ? a->mn : b->mn;
        for (size_t i = 0; i < m; i++) {
            JMem *pa = &a->mem[ia[i]];
            JMem *pb = &b->mem[ib[i]];
            int c = cmp_bytes(pa->key, pa->klen, pb->key, pb->klen);
            if (c) {
                free(ia);
                free(ib);
                return c;
            }
        }
        if (a->mn != b->mn) {
            int r = a->mn < b->mn ? -1 : 1;
            free(ia);
            free(ib);
            return r;
        }
        for (size_t i = 0; i < a->mn; i++) {
            int c = compare_values(a->mem[ia[i]].val, b->mem[ib[i]].val);
            if (c) {
                free(ia);
                free(ib);
                return c;
            }
        }
        free(ia);
        free(ib);
        return 0;
    }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Errors and argument helpers                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    char        code[32];
    const char *step;   /* points at step id, or NULL */
    char        msg[256];
} Err;

static int seterr(Err *e, const char *code, const char *fmt, ...) {
    snprintf(e->code, sizeof e->code, "%s", code);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->msg, sizeof e->msg, fmt, ap);
    va_end(ap);
    return -1;
}

static JVal *errv(Err *e, const char *code, const char *fmt, ...) {
    snprintf(e->code, sizeof e->code, "%s", code);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->msg, sizeof e->msg, fmt, ap);
    va_end(ap);
    return NULL;
}

static int a_unknown(const JVal *args, const char *const *allowed, Err *e) {
    for (size_t i = 0; i < args->mn; i++) {
        int ok = 0;
        for (const char *const *p = allowed; *p; p++) {
            if (strlen(*p) == args->mem[i].klen &&
                memcmp(*p, args->mem[i].key, args->mem[i].klen) == 0) {
                ok = 1;
                break;
            }
        }
        if (!ok)
            return seterr(e, "INVALID_ARG", "unknown argument '%.*s'",
                          (int)args->mem[i].klen, args->mem[i].key);
    }
    return 0;
}

static int a_need_i64(const JVal *args, const char *name, int64_t *out, Err *e) {
    JVal *v = o_get(args, name);
    if (!v) return seterr(e, "INVALID_ARG", "missing argument '%s'", name);
    if (!is_i64(v))
        return seterr(e, "INVALID_ARG", "argument '%s' must be an integer", name);
    *out = v->i;
    return 0;
}

static int a_opt_i64(const JVal *args, const char *name, int64_t *out, int *present, Err *e) {
    JVal *v = o_get(args, name);
    if (!v) {
        *present = 0;
        return 0;
    }
    if (!is_i64(v))
        return seterr(e, "INVALID_ARG", "argument '%s' must be an integer", name);
    *present = 1;
    *out = v->i;
    return 0;
}

static int a_need_str(const JVal *args, const char *name, const char **out, size_t *len, Err *e) {
    JVal *v = o_get(args, name);
    if (!v) return seterr(e, "INVALID_ARG", "missing argument '%s'", name);
    if (v->t != J_STR)
        return seterr(e, "INVALID_ARG", "argument '%s' must be a string", name);
    *out = v->s;
    *len = v->slen;
    return 0;
}

static int a_need_val(const JVal *args, const char *name, JVal **out, Err *e) {
    JVal *v = o_get(args, name);
    if (!v) return seterr(e, "INVALID_ARG", "missing argument '%s'", name);
    *out = v;
    return 0;
}

static int a_need_arr(const JVal *args, const char *name, JVal **out, Err *e) {
    JVal *v = o_get(args, name);
    if (!v) return seterr(e, "INVALID_ARG", "missing argument '%s'", name);
    if (v->t != J_ARR)
        return seterr(e, "INVALID_ARG", "argument '%s' must be an array", name);
    *out = v;
    return 0;
}

static int a_need_obj(const JVal *args, const char *name, JVal **out, Err *e) {
    JVal *v = o_get(args, name);
    if (!v) return seterr(e, "INVALID_ARG", "missing argument '%s'", name);
    if (v->t != J_OBJ)
        return seterr(e, "INVALID_ARG", "argument '%s' must be an object", name);
    *out = v;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Arithmetic helpers                                                  */
/* ------------------------------------------------------------------ */

static int bin_compute(const char *op, int64_t a, int64_t b, int64_t *r, Err *e) {
    if (strcmp(op, "add") == 0) {
        if (__builtin_add_overflow(a, b, r))
            return seterr(e, "ARITHMETIC_OVERFLOW", "add overflow");
        return 0;
    }
    if (strcmp(op, "subtract") == 0) {
        if (__builtin_sub_overflow(a, b, r))
            return seterr(e, "ARITHMETIC_OVERFLOW", "subtract overflow");
        return 0;
    }
    if (strcmp(op, "multiply") == 0) {
        if (__builtin_mul_overflow(a, b, r))
            return seterr(e, "ARITHMETIC_OVERFLOW", "multiply overflow");
        return 0;
    }
    if (strcmp(op, "divide") == 0) {
        if (b == 0) return seterr(e, "DIVISION_BY_ZERO", "division by zero");
        if (a == INT64_MIN && b == -1)
            return seterr(e, "ARITHMETIC_OVERFLOW", "INT64_MIN / -1");
        *r = a / b;
        return 0;
    }
    if (strcmp(op, "mod") == 0) {
        if (b == 0) return seterr(e, "DIVISION_BY_ZERO", "mod by zero");
        if (a == INT64_MIN && b == -1)
            return seterr(e, "ARITHMETIC_OVERFLOW", "INT64_MIN %% -1");
        *r = a % b;
        return 0;
    }
    return seterr(e, "INVALID_ARG", "unsupported operator '%s'", op);
}

/* ------------------------------------------------------------------ */
/* Operations                                                          */
/* ------------------------------------------------------------------ */

static JVal *op_bin2(const char *name, const JVal *args, Err *e) {
    static const char *const al[] = {"a", "b", NULL};
    if (a_unknown(args, al, e)) return NULL;
    int64_t a, b, r;
    if (a_need_i64(args, "a", &a, e)) return NULL;
    if (a_need_i64(args, "b", &b, e)) return NULL;
    if (bin_compute(name, a, b, &r, e)) return NULL;
    return jint(r);
}

static JVal *op_sum(const JVal *args, Err *e) {
    static const char *const al[] = {"input", NULL};
    if (a_unknown(args, al, e)) return NULL;
    JVal *in;
    if (a_need_arr(args, "input", &in, e)) return NULL;
    int64_t acc = 0;
    for (size_t i = 0; i < in->n; i++) {
        if (!is_i64(in->it[i]))
            return errv(e, "INVALID_ARG", "sum: element %zu is not an integer", i);
        if (__builtin_add_overflow(acc, in->it[i]->i, &acc))
            return errv(e, "ARITHMETIC_OVERFLOW", "sum overflow");
    }
    return jint(acc);
}

static JVal *op_minmax(const char *which, const JVal *args, Err *e) {
    static const char *const al[] = {"input", NULL};
    if (a_unknown(args, al, e)) return NULL;
    JVal *in;
    if (a_need_arr(args, "input", &in, e)) return NULL;
    if (in->n == 0)
        return errv(e, "EMPTY_ARRAY", "%s of empty array", which);
    for (size_t i = 0; i < in->n; i++) {
        if (!is_i64(in->it[i]))
            return errv(e, "INVALID_ARG", "%s: element %zu is not an integer", which, i);
    }
    int64_t best = in->it[0]->i;
    for (size_t i = 1; i < in->n; i++) {
        int64_t x = in->it[i]->i;
        if (strcmp(which, "min") == 0) {
            if (x < best) best = x;
        } else {
            if (x > best) best = x;
        }
    }
    return jint(best);
}

#define RANGE_LIMIT 20000000LL

static JVal *op_range(const JVal *args, Err *e) {
    static const char *const al[] = {"start", "end", NULL};
    if (a_unknown(args, al, e)) return NULL;
    int64_t start, end;
    if (a_need_i64(args, "start", &start, e)) return NULL;
    if (a_need_i64(args, "end", &end, e)) return NULL;
    JVal *out = jarr();
    if (start > end) return out;
    uint64_t span = (uint64_t)end - (uint64_t)start;
    if (span >= (uint64_t)RANGE_LIMIT)
        return errv(e, "INVALID_ARG", "range too large (%" PRIu64 " elements)",
                    span + 1u);
    uint64_t cnt = span + 1u;
    out->it = xmalloc((size_t)cnt * sizeof(JVal *));
    out->cap = (size_t)cnt;
    for (uint64_t k = 0; k < cnt; k++) {
        int64_t v = (int64_t)((uint64_t)start + k);
        out->it[out->n++] = jint(v);
    }
    return out;
}

static JVal *op_concat(const JVal *args, Err *e) {
    static const char *const al[] = {"a", "b", NULL};
    if (a_unknown(args, al, e)) return NULL;
    const char *a, *b;
    size_t an, bn;
    if (a_need_str(args, "a", &a, &an, e)) return NULL;
    if (a_need_str(args, "b", &b, &bn, e)) return NULL;
    char *p = xmalloc(an + bn + 1);
    if (an) memcpy(p, a, an);
    if (bn) memcpy(p + an, b, bn);
    p[an + bn] = '\0';
    return jstr_take(p, an + bn);
}

/* number of code points in a UTF-8 byte string */
static size_t cp_count(const char *s, size_t n) {
    size_t c = 0;
    for (size_t i = 0; i < n; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) c++;
    }
    return c;
}

static JVal *op_split(const JVal *args, Err *e) {
    static const char *const al[] = {"input", "sep", NULL};
    if (a_unknown(args, al, e)) return NULL;
    const char *in, *sep;
    size_t inl, sepl;
    if (a_need_str(args, "input", &in, &inl, e)) return NULL;
    if (a_need_str(args, "sep", &sep, &sepl, e)) return NULL;
    JVal *out = jarr();
    if (sepl == 0) {
        size_t i = 0;
        while (i < inl) {
            size_t j = i + 1;
            while (j < inl && ((unsigned char)in[j] & 0xC0) == 0x80) j++;
            a_push(out, jstr(in + i, j - i));
            i = j;
        }
        return out;
    }
    if (inl == 0) {
        a_push(out, jstr("", 0));
        return out;
    }
    size_t pos = 0;
    for (;;) {
        size_t found = (size_t)-1;
        for (size_t k = pos; k + sepl <= inl; k++) {
            if (memcmp(in + k, sep, sepl) == 0) {
                found = k;
                break;
            }
        }
        if (found == (size_t)-1) {
            a_push(out, jstr(in + pos, inl - pos));
            break;
        }
        a_push(out, jstr(in + pos, found - pos));
        pos = found + sepl;
        if (pos == inl) {
            a_push(out, jstr("", 0));
            break;
        }
    }
    return out;
}

static JVal *op_join(const JVal *args, Err *e) {
    static const char *const al[] = {"input", "sep", NULL};
    if (a_unknown(args, al, e)) return NULL;
    JVal *in;
    const char *sep;
    size_t sepl;
    if (a_need_arr(args, "input", &in, e)) return NULL;
    if (a_need_str(args, "sep", &sep, &sepl, e)) return NULL;
    size_t total = 0;
    for (size_t i = 0; i < in->n; i++) {
        if (in->it[i]->t != J_STR)
            return errv(e, "INVALID_ARG", "join: element %zu is not a string", i);
        total += in->it[i]->slen;
        if (i) total += sepl;
    }
    char *p = xmalloc(total + 1);
    size_t off = 0;
    for (size_t i = 0; i < in->n; i++) {
        if (i) {
            if (sepl) memcpy(p + off, sep, sepl);
            off += sepl;
        }
        if (in->it[i]->slen) memcpy(p + off, in->it[i]->s, in->it[i]->slen);
        off += in->it[i]->slen;
    }
    p[off] = '\0';
    return jstr_take(p, off);
}

static JVal *op_case(const char *which, const JVal *args, Err *e) {
    static const char *const al[] = {"input", NULL};
    if (a_unknown(args, al, e)) return NULL;
    const char *s;
    size_t n;
    if (a_need_str(args, "input", &s, &n, e)) return NULL;
    char *p = xmalloc(n + 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (strcmp(which, "upper") == 0) {
            if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
        } else {
            if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        }
        p[i] = (char)c;
    }
    p[n] = '\0';
    return jstr_take(p, n);
}

static int is_ascii_ws(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static JVal *op_trim(const JVal *args, Err *e) {
    static const char *const al[] = {"input", NULL};
    if (a_unknown(args, al, e)) return NULL;
    const char *s;
    size_t n;
    if (a_need_str(args, "input", &s, &n, e)) return NULL;
    size_t a = 0, b = n;
    while (a < b && is_ascii_ws((unsigned char)s[a])) a++;
    while (b > a && is_ascii_ws((unsigned char)s[b - 1])) b--;
    return jstr(s + a, b - a);
}

static JVal *op_replace(const JVal *args, Err *e) {
    static const char *const al[] = {"input", "search", "replace", NULL};
    if (a_unknown(args, al, e)) return NULL;
    const char *in, *search, *rep;
    size_t inl, sl, rl;
    if (a_need_str(args, "input", &in, &inl, e)) return NULL;
    if (a_need_str(args, "search", &search, &sl, e)) return NULL;
    if (a_need_str(args, "replace", &rep, &rl, e)) return NULL;
    if (sl == 0)
        return errv(e, "INVALID_ARG", "replace: empty search string");
    Buf b;
    memset(&b, 0, sizeof b);
    size_t pos = 0;
    for (;;) {
        size_t found = (size_t)-1;
        for (size_t k = pos; k + sl <= inl; k++) {
            if (memcmp(in + k, search, sl) == 0) {
                found = k;
                break;
            }
        }
        if (found == (size_t)-1) {
            bput(&b, in + pos, inl - pos);
            break;
        }
        bput(&b, in + pos, found - pos);
        bput(&b, rep, rl);
        pos = found + sl;
    }
    return jstr_from_buf(&b);
}

static JVal *op_length(const JVal *args, Err *e) {
    static const char *const al[] = {"input", NULL};
    if (a_unknown(args, al, e)) return NULL;
    const char *s;
    size_t n;
    if (a_need_str(args, "input", &s, &n, e)) return NULL;
    return jint((int64_t)cp_count(s, n));
}

/* ---- map transform ---- */

static int is_self_marker(const JVal *v) {
    return v && v->t == J_OBJ && v->mn == 1 &&
           v->mem[0].klen == 5 &&
           memcmp(v->mem[0].key, "$self", 5) == 0 &&
           v->mem[0].val->t == J_BOOL && v->mem[0].val->b;
}

/* Structural validation of a map transform descriptor (independent of the
 * element type), so that malformed descriptors are rejected even for an
 * empty input array. */
static int tf_check(const JVal *tr, Err *e) {
    if (!tr || tr->t != J_OBJ)
        return seterr(e, "INVALID_ARG", "transform must be an object");
    JVal *tv = o_get(tr, "type");
    if (!tv || tv->t != J_STR)
        return seterr(e, "INVALID_ARG", "transform 'type' must be a string");
    const char *t = tv->s;
    static const char *const al_bin[] = {"type", "value", NULL};
    static const char *const al_noarg[] = {"type", NULL};
    static const char *const al_op[] = {"type", "op", "args", NULL};

    if (strcmp(t, "constant") == 0) {
        if (a_unknown(tr, al_bin, e)) return -1;
        if (!o_get(tr, "value"))
            return seterr(e, "INVALID_ARG", "constant transform requires 'value'");
        return 0;
    }
    if (strcmp(t, "add") == 0 || strcmp(t, "subtract") == 0 ||
        strcmp(t, "multiply") == 0 || strcmp(t, "divide") == 0 ||
        strcmp(t, "mod") == 0) {
        if (a_unknown(tr, al_bin, e)) return -1;
        int64_t k;
        return a_need_i64(tr, "value", &k, e);
    }
    if (strcmp(t, "negate") == 0 || strcmp(t, "abs") == 0 ||
        strcmp(t, "stringify") == 0 || strcmp(t, "type") == 0)
        return a_unknown(tr, al_noarg, e);
    if (strcmp(t, "op") == 0) {
        if (a_unknown(tr, al_op, e)) return -1;
        JVal *opv = o_get(tr, "op");
        if (!opv || opv->t != J_STR)
            return seterr(e, "INVALID_ARG", "'op' transform requires string 'op'");
        const char *opn = opv->s;
        if (strcmp(opn, "add") && strcmp(opn, "subtract") &&
            strcmp(opn, "multiply") && strcmp(opn, "divide") &&
            strcmp(opn, "mod"))
            return seterr(e, "INVALID_ARG", "unsupported transform op '%s'", opn);
        JVal *av = o_get(tr, "args");
        if (!av || av->t != J_OBJ)
            return seterr(e, "INVALID_ARG", "'op' transform requires object 'args'");
        static const char *const al_ab[] = {"a", "b", NULL};
        if (a_unknown(av, al_ab, e)) return -1;
        JVal *a = o_get(av, "a");
        JVal *b = o_get(av, "b");
        if (!a || !b)
            return seterr(e, "INVALID_ARG", "'op' transform requires 'a' and 'b'");
        int aself = is_self_marker(a);
        int bself = is_self_marker(b);
        if (aself == bself)
            return seterr(e, "INVALID_ARG", "exactly one operand must be $self");
        JVal *other = aself ? b : a;
        if (!is_i64(other))
            return seterr(e, "INVALID_ARG", "transform operand must be an integer");
        return 0;
    }
    return seterr(e, "INVALID_ARG", "unknown transform type '%s'", t);
}

static JVal *apply_transform(const JVal *tr, const JVal *el, Err *e) {
    if (tf_check(tr, e)) return NULL;
    if (!tr || tr->t != J_OBJ)
        return errv(e, "INVALID_ARG", "transform must be an object");
    JVal *tv = o_get(tr, "type");
    if (!tv || tv->t != J_STR)
        return errv(e, "INVALID_ARG", "transform 'type' must be a string");
    const char *t = tv->s;

    static const char *const al_bin[] = {"type", "value", NULL};
    static const char *const al_noarg[] = {"type", NULL};
    static const char *const al_op[] = {"type", "op", "args", NULL};

    if (strcmp(t, "constant") == 0) {
        if (a_unknown(tr, al_bin, e)) return NULL;
        JVal *val = o_get(tr, "value");
        if (!val) return errv(e, "INVALID_ARG", "constant transform requires 'value'");
        return val;
    }
    if (strcmp(t, "add") == 0 || strcmp(t, "subtract") == 0 ||
        strcmp(t, "multiply") == 0 || strcmp(t, "divide") == 0 ||
        strcmp(t, "mod") == 0) {
        if (a_unknown(tr, al_bin, e)) return NULL;
        int64_t k, r;
        if (!is_i64(el))
            return errv(e, "INVALID_ARG", "map: element is not an integer");
        if (a_need_i64(tr, "value", &k, e)) return NULL;
        if (bin_compute(t, el->i, k, &r, e)) return NULL;
        return jint(r);
    }
    if (strcmp(t, "negate") == 0) {
        if (a_unknown(tr, al_noarg, e)) return NULL;
        if (!is_i64(el))
            return errv(e, "INVALID_ARG", "map: element is not an integer");
        if (el->i == INT64_MIN)
            return errv(e, "ARITHMETIC_OVERFLOW", "negate overflow");
        return jint(-el->i);
    }
    if (strcmp(t, "abs") == 0) {
        if (a_unknown(tr, al_noarg, e)) return NULL;
        if (!is_i64(el))
            return errv(e, "INVALID_ARG", "map: element is not an integer");
        if (el->i == INT64_MIN)
            return errv(e, "ARITHMETIC_OVERFLOW", "abs overflow");
        return jint(el->i < 0 ? -el->i : el->i);
    }
    if (strcmp(t, "stringify") == 0) {
        if (a_unknown(tr, al_noarg, e)) return NULL;
        Buf b;
        memset(&b, 0, sizeof b);
        wjson(&b, el);
        return jstr_from_buf(&b);
    }
    if (strcmp(t, "type") == 0) {
        if (a_unknown(tr, al_noarg, e)) return NULL;
        return jstr(type_name(el), strlen(type_name(el)));
    }
    if (strcmp(t, "op") == 0) {
        if (a_unknown(tr, al_op, e)) return NULL;
        JVal *opv = o_get(tr, "op");
        if (!opv || opv->t != J_STR)
            return errv(e, "INVALID_ARG", "'op' transform requires string 'op'");
        const char *opn = opv->s;
        if (strcmp(opn, "add") && strcmp(opn, "subtract") &&
            strcmp(opn, "multiply") && strcmp(opn, "divide") &&
            strcmp(opn, "mod"))
            return errv(e, "INVALID_ARG", "unsupported transform op '%s'", opn);
        JVal *av = o_get(tr, "args");
        if (!av || av->t != J_OBJ)
            return errv(e, "INVALID_ARG", "'op' transform requires object 'args'");
        static const char *const al_ab[] = {"a", "b", NULL};
        if (a_unknown(av, al_ab, e)) return NULL;
        JVal *a = o_get(av, "a");
        JVal *b = o_get(av, "b");
        if (!a || !b)
            return errv(e, "INVALID_ARG", "'op' transform requires 'a' and 'b'");
        int aself = is_self_marker(a);
        int bself = is_self_marker(b);
        if (aself == bself)
            return errv(e, "INVALID_ARG", "exactly one operand must be $self");
        JVal *other = aself ? b : a;
        if (!is_i64(other))
            return errv(e, "INVALID_ARG", "transform operand must be an integer");
        if (!is_i64(el))
            return errv(e, "INVALID_ARG", "map: element is not an integer");
        int64_t lhs = aself ? el->i : other->i;
        int64_t rhs = aself ? other->i : el->i;
        int64_t r;
        if (bin_compute(opn, lhs, rhs, &r, e)) return NULL;
        return jint(r);
    }
    return errv(e, "INVALID_ARG", "unknown transform type '%s'", t);
}

/* ---- filter predicate ---- */

static int is_type_name(const char *s) {
    return !strcmp(s, "null") || !strcmp(s, "boolean") || !strcmp(s, "number") ||
           !strcmp(s, "string") || !strcmp(s, "array") || !strcmp(s, "object");
}

/* Type name carried by a `type_is` predicate: accepted under "value",
 * "type_name", or as a second "type" member (as literally shown in SPEC). */
static const char *type_is_want(const JVal *p) {
    const char *want = NULL;
    for (size_t i = 0; i < p->mn; i++) {
        JMem *m = &p->mem[i];
        if (m->val->t != J_STR) continue;
        if (m->klen == 5 && !memcmp(m->key, "value", 5)) want = m->val->s;
        else if (m->klen == 9 && !memcmp(m->key, "type_name", 9)) want = m->val->s;
        else if (i > 0 && m->klen == 4 && !memcmp(m->key, "type", 4) &&
                 is_type_name(m->val->s))
            want = m->val->s;
    }
    return want;
}

/* Structural validation of a filter predicate descriptor (independent of the
 * element type). */
static int pr_check(const JVal *p, Err *e) {
    if (p->t == J_STR) {
        if (!strcmp(p->s, "even") || !strcmp(p->s, "odd")) return 0;
        return seterr(e, "INVALID_ARG", "unknown predicate '%s'", p->s);
    }
    if (p->t != J_OBJ)
        return seterr(e, "INVALID_ARG", "predicate must be an object");
    JVal *tv = o_get(p, "type");
    if (!tv || tv->t != J_STR)
        return seterr(e, "INVALID_ARG", "predicate 'type' must be a string");
    const char *t = tv->s;
    static const char *const al_noarg[] = {"type", NULL};
    static const char *const al_val[] = {"type", "value", NULL};
    static const char *const al_typeis[] = {"type", "value", "type_name", NULL};

    if (!strcmp(t, "even") || !strcmp(t, "odd") ||
        !strcmp(t, "truthy") || !strcmp(t, "falsy"))
        return a_unknown(p, al_noarg, e);
    if (!strcmp(t, "gt") || !strcmp(t, "gte") || !strcmp(t, "lt") ||
        !strcmp(t, "lte") || !strcmp(t, "eq") || !strcmp(t, "ne")) {
        if (a_unknown(p, al_val, e)) return -1;
        int64_t v;
        return a_need_i64(p, "value", &v, e);
    }
    if (!strcmp(t, "type_is")) {
        if (a_unknown(p, al_typeis, e)) return -1;
        if (!type_is_want(p))
            return seterr(e, "INVALID_ARG", "type_is predicate requires a type name");
        return 0;
    }
    if (!strcmp(t, "string_contains")) {
        if (a_unknown(p, al_val, e)) return -1;
        const char *needle;
        size_t nl;
        return a_need_str(p, "value", &needle, &nl, e);
    }
    return seterr(e, "INVALID_ARG", "unknown predicate type '%s'", t);
}

static int pred_eval(const JVal *p, const JVal *el, int *out, Err *e) {
    if (pr_check(p, e)) return -1;
    if (p->t == J_STR) {
        /* tolerant shorthand: "even" / "odd" */
        if (!strcmp(p->s, "even")) {
            if (!is_i64(el))
                return seterr(e, "INVALID_ARG", "filter: element is not an integer");
            *out = (el->i % 2) == 0;
            return 0;
        }
        if (!strcmp(p->s, "odd")) {
            if (!is_i64(el))
                return seterr(e, "INVALID_ARG", "filter: element is not an integer");
            *out = (el->i % 2) != 0;
            return 0;
        }
        return seterr(e, "INVALID_ARG", "unknown predicate '%s'", p->s);
    }
    if (p->t != J_OBJ)
        return seterr(e, "INVALID_ARG", "predicate must be an object");
    JVal *tv = o_get(p, "type");
    if (!tv || tv->t != J_STR)
        return seterr(e, "INVALID_ARG", "predicate 'type' must be a string");
    const char *t = tv->s;

    static const char *const al_noarg[] = {"type", NULL};
    static const char *const al_val[] = {"type", "value", NULL};

    if (!strcmp(t, "even") || !strcmp(t, "odd")) {
        if (a_unknown(p, al_noarg, e)) return -1;
        if (!is_i64(el))
            return seterr(e, "INVALID_ARG", "filter: element is not an integer");
        *out = (el->i % 2) == 0;
        if (!strcmp(t, "odd")) *out = !*out;
        return 0;
    }
    if (!strcmp(t, "gt") || !strcmp(t, "gte") || !strcmp(t, "lt") || !strcmp(t, "lte")) {
        if (a_unknown(p, al_val, e)) return -1;
        int64_t v;
        if (a_need_i64(p, "value", &v, e)) return -1;
        if (el->t != J_NUM)
            return seterr(e, "INVALID_ARG", "filter: element is not a number");
        int c = compare_values(el, jint(v));
        if (!strcmp(t, "gt")) *out = c > 0;
        else if (!strcmp(t, "gte")) *out = c >= 0;
        else if (!strcmp(t, "lt")) *out = c < 0;
        else *out = c <= 0;
        return 0;
    }
    if (!strcmp(t, "eq") || !strcmp(t, "ne")) {
        if (a_unknown(p, al_val, e)) return -1;
        int64_t v;
        if (a_need_i64(p, "value", &v, e)) return -1;
        int eq = jequal(el, jint(v));
        *out = !strcmp(t, "eq") ? eq : !eq;
        return 0;
    }
    if (!strcmp(t, "truthy") || !strcmp(t, "falsy")) {
        if (a_unknown(p, al_noarg, e)) return -1;
        *out = truthy(el);
        if (!strcmp(t, "falsy")) *out = !*out;
        return 0;
    }
    if (!strcmp(t, "type_is")) {
        const char *want = type_is_want(p);
        if (!want)
            return seterr(e, "INVALID_ARG", "type_is predicate requires a type name");
        *out = strcmp(type_name(el), want) == 0;
        return 0;
    }
    if (!strcmp(t, "string_contains")) {
        if (a_unknown(p, al_val, e)) return -1;
        const char *needle;
        size_t nl;
        if (a_need_str(p, "value", &needle, &nl, e)) return -1;
        if (el->t != J_STR)
            return seterr(e, "INVALID_ARG", "string_contains: element is not a string");
        int found = 0;
        if (nl == 0) {
            found = 1;
        } else if (el->slen >= nl) {
            for (size_t i = 0; i + nl <= el->slen; i++) {
                if (memcmp(el->s + i, needle, nl) == 0) {
                    found = 1;
                    break;
                }
            }
        }
        *out = found;
        return 0;
    }
    return seterr(e, "INVALID_ARG", "unknown predicate type '%s'", t);
}

static JVal *op_map(const JVal *args, Err *e) {
    static const char *const al[] = {"input", "transform", NULL};
    if (a_unknown(args, al, e)) return NULL;
    JVal *in, *tr;
    if (a_need_arr(args, "input", &in, e)) return NULL;
    if (a_need_obj(args, "transform", &tr, e)) return NULL;
    if (tf_check(tr, e)) return NULL;
    JVal *out = jarr();
    for (size_t i = 0; i < in->n; i++) {
        JVal *r = apply_transform(tr, in->it[i], e);
        if (!r) return NULL;
        a_push(out, r);
    }
    return out;
}

static JVal *op_filter(const JVal *args, Err *e) {
    static const char *const al[] = {"input", "predicate", NULL};
    if (a_unknown(args, al, e)) return NULL;
    JVal *in = NULL, *pr = NULL;
    if (a_need_arr(args, "input", &in, e)) return NULL;
    if (a_need_val(args, "predicate", &pr, e)) return NULL;
    if (pr_check(pr, e)) return NULL;
    JVal *out = jarr();
    for (size_t i = 0; i < in->n; i++) {
        int keep = 0;
        if (pred_eval(pr, in->it[i], &keep, e)) return NULL;
        if (keep) a_push(out, in->it[i]);
    }
    return out;
}

static JVal *op_reduce(const JVal *args, Err *e) {
    static const char *const al[] = {"input", "op", "initial", NULL};
    if (a_unknown(args, al, e)) return NULL;
    JVal *in;
    const char *opn;
    size_t opnl;
    if (a_need_arr(args, "input", &in, e)) return NULL;
    if (a_need_str(args, "op", &opn, &opnl, e)) return NULL;
    if (!(opnl == 3 && !memcmp(opn, "add", 3)) &&
        !(opnl == 8 && !memcmp(opn, "subtract", 8)) &&
        !(opnl == 8 && !memcmp(opn, "multiply", 8)))
        return errv(e, "INVALID_ARG", "reduce: unsupported op '%.*s'", (int)opnl, opn);
    int64_t acc = 0;
    int present = 0;
    if (a_opt_i64(args, "initial", &acc, &present, e)) return NULL;
    if (!present)
        acc = (opnl == 8 && !memcmp(opn, "multiply", 8)) ? 1 : 0;
    for (size_t i = 0; i < in->n; i++) {
        if (!is_i64(in->it[i]))
            return errv(e, "INVALID_ARG", "reduce: element %zu is not an integer", i);
        if (bin_compute(opn, acc, in->it[i]->i, &acc, e)) return NULL;
    }
    return jint(acc);
}

typedef struct {
    const JVal *v;
    size_t      idx;
} SortItem;

static int cmp_sortitem(const void *pa, const void *pb) {
    const SortItem *a = pa, *b = pb;
    int c = compare_values(a->v, b->v);
    if (c) return c;
    if (a->idx == b->idx) return 0;
    return a->idx < b->idx ? -1 : 1;
}

static JVal *op_sort(const JVal *args, Err *e) {
    static const char *const al[] = {"input", NULL};
    if (a_unknown(args, al, e)) return NULL;
    JVal *in;
    if (a_need_arr(args, "input", &in, e)) return NULL;
    SortItem *items = xmalloc((in->n ? in->n : 1) * sizeof(SortItem));
    for (size_t i = 0; i < in->n; i++) {
        items[i].v = in->it[i];
        items[i].idx = i;
    }
    if (in->n > 1) qsort(items, in->n, sizeof(SortItem), cmp_sortitem);
    JVal *out = jarr();
    for (size_t i = 0; i < in->n; i++) a_push(out, (JVal *)items[i].v);
    free(items);
    return out;
}

static JVal *op_reverse(const JVal *args, Err *e) {
    static const char *const al[] = {"input", NULL};
    if (a_unknown(args, al, e)) return NULL;
    JVal *in;
    if (a_need_arr(args, "input", &in, e)) return NULL;
    JVal *out = jarr();
    for (size_t i = in->n; i > 0; i--) a_push(out, in->it[i - 1]);
    return out;
}

static uint64_t hash_bytes(const char *s, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ull;
    }
    return h;
}

static uint64_t mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

/* Structural hash: equal values (per jequal) hash equally. */
static uint64_t jhash(const JVal *v) {
    switch (v->t) {
    case J_NULL:
        return mix64(1);
    case J_BOOL:
        return mix64(2u + (uint64_t)v->b);
    case J_NUM: {
        double dv = (double)v->d;
        uint64_t bits;
        if (dv == 0) bits = 0;
        else memcpy(&bits, &dv, sizeof bits);
        return mix64(bits ^ 0x9e3779b97f4a7c15ull);
    }
    case J_STR:
        return mix64(hash_bytes(v->s, v->slen) ^ 3u);
    case J_ARR: {
        uint64_t h = 0x517cc1b727220a95ull;
        for (size_t i = 0; i < v->n; i++) h = mix64(h ^ jhash(v->it[i]));
        return mix64(h + (uint64_t)v->n);
    }
    case J_OBJ: {
        uint64_t h = 0;
        for (size_t i = 0; i < v->mn; i++) {
            uint64_t kh = hash_bytes(v->mem[i].key, v->mem[i].klen);
            h += mix64(kh ^ jhash(v->mem[i].val));
        }
        return mix64(h ^ ((uint64_t)v->mn + 0x2545f4914f6cdd1dull));
    }
    }
    return 0;
}

typedef struct HNode {
    uint64_t       h;
    const JVal    *v;
    struct HNode  *next;
} HNode;

static JVal *op_unique(const JVal *args, Err *e) {
    static const char *const al[] = {"input", NULL};
    if (a_unknown(args, al, e)) return NULL;
    JVal *in;
    if (a_need_arr(args, "input", &in, e)) return NULL;
    size_t cap = 16;
    while (cap < in->n * 2 + 1) cap *= 2;
    HNode **tab = xcalloc(cap, sizeof(HNode *));
    JVal *out = jarr();
    for (size_t i = 0; i < in->n; i++) {
        uint64_t h = jhash(in->it[i]);
        size_t b = (size_t)(h & (uint64_t)(cap - 1));
        int dup = 0;
        for (HNode *nd = tab[b]; nd; nd = nd->next) {
            if (nd->h == h && jequal(nd->v, in->it[i])) {
                dup = 1;
                break;
            }
        }
        if (!dup) {
            HNode *nd = xmalloc(sizeof *nd);
            nd->h = h;
            nd->v = in->it[i];
            nd->next = tab[b];
            tab[b] = nd;
            a_push(out, in->it[i]);
        }
    }
    free(tab);
    return out;
}

static JVal *op_slice(const JVal *args, Err *e) {
    static const char *const al[] = {"input", "start", "end", NULL};
    if (a_unknown(args, al, e)) return NULL;
    JVal *in = NULL;
    int64_t start = 0, end = 0;
    int present = 0;
    if (a_need_arr(args, "input", &in, e)) return NULL;
    if (a_need_i64(args, "start", &start, e)) return NULL;
    if (a_opt_i64(args, "end", &end, &present, e)) return NULL;
    int64_t n = (int64_t)in->n;
    if (!present) end = n;
    if (start < 0) start += n;
    if (start < 0) start = 0;
    if (start > n) start = n;
    if (end < 0) end += n;
    if (end < 0) end = 0;
    if (end > n) end = n;
    JVal *out = jarr();
    if (start >= end) return out;
    for (int64_t i = start; i < end; i++) a_push(out, in->it[i]);
    return out;
}

static JVal *op_flatten(const JVal *args, Err *e) {
    static const char *const al[] = {"input", NULL};
    if (a_unknown(args, al, e)) return NULL;
    JVal *in;
    if (a_need_arr(args, "input", &in, e)) return NULL;
    JVal *out = jarr();
    for (size_t i = 0; i < in->n; i++) {
        JVal *el = in->it[i];
        if (el->t == J_ARR) {
            for (size_t j = 0; j < el->n; j++) a_push(out, el->it[j]);
        } else {
            a_push(out, el);
        }
    }
    return out;
}

static JVal *op_get(const JVal *args, Err *e) {
    static const char *const al[] = {"input", "key", NULL};
    if (a_unknown(args, al, e)) return NULL;
    JVal *in;
    const char *key;
    size_t kl;
    if (a_need_obj(args, "input", &in, e)) return NULL;
    if (a_need_str(args, "key", &key, &kl, e)) return NULL;
    for (size_t i = 0; i < in->mn; i++) {
        if (in->mem[i].klen == kl && memcmp(in->mem[i].key, key, kl) == 0)
            return in->mem[i].val;
    }
    return jnull();
}

static JVal *op_set(const JVal *args, Err *e) {
    static const char *const al[] = {"input", "key", "value", NULL};
    if (a_unknown(args, al, e)) return NULL;
    JVal *in = NULL, *val = NULL;
    const char *key;
    size_t kl;
    if (a_need_obj(args, "input", &in, e)) return NULL;
    if (a_need_str(args, "key", &key, &kl, e)) return NULL;
    if (a_need_val(args, "value", &val, e)) return NULL;
    JVal *out = jobj();
    for (size_t i = 0; i < in->mn; i++) {
        if (in->mem[i].klen == kl && memcmp(in->mem[i].key, key, kl) == 0)
            o_put(out, key, kl, val);
        else
            o_put(out, in->mem[i].key, in->mem[i].klen, in->mem[i].val);
    }
    o_put(out, key, kl, val);
    return out;
}

static void sorted_key_indices(const JVal *o, size_t **out) {
    size_t *idx = xmalloc((o->mn ? o->mn : 1) * sizeof(size_t));
    for (size_t i = 0; i < o->mn; i++) idx[i] = i;
    sort_member_idx(o, idx);
    *out = idx;
}

static JVal *op_keys(const JVal *args, Err *e) {
    static const char *const al[] = {"input", NULL};
    if (a_unknown(args, al, e)) return NULL;
    JVal *in;
    if (a_need_obj(args, "input", &in, e)) return NULL;
    size_t *idx;
    sorted_key_indices(in, &idx);
    JVal *out = jarr();
    for (size_t i = 0; i < in->mn; i++)
        a_push(out, jstr(in->mem[idx[i]].key, in->mem[idx[i]].klen));
    free(idx);
    return out;
}

static JVal *op_values(const JVal *args, Err *e) {
    static const char *const al[] = {"input", NULL};
    if (a_unknown(args, al, e)) return NULL;
    JVal *in;
    if (a_need_obj(args, "input", &in, e)) return NULL;
    size_t *idx;
    sorted_key_indices(in, &idx);
    JVal *out = jarr();
    for (size_t i = 0; i < in->mn; i++)
        a_push(out, in->mem[idx[i]].val);
    free(idx);
    return out;
}

static JVal *op_merge(const JVal *args, Err *e) {
    static const char *const al[] = {"a", "b", NULL};
    if (a_unknown(args, al, e)) return NULL;
    JVal *a, *b;
    if (a_need_obj(args, "a", &a, e)) return NULL;
    if (a_need_obj(args, "b", &b, e)) return NULL;
    JVal *out = jobj();
    for (size_t i = 0; i < a->mn; i++)
        o_put(out, a->mem[i].key, a->mem[i].klen, a->mem[i].val);
    for (size_t i = 0; i < b->mn; i++)
        o_put(out, b->mem[i].key, b->mem[i].klen, b->mem[i].val);
    return out;
}

static JVal *op_pick(const JVal *args, Err *e) {
    static const char *const al[] = {"input", "keys", NULL};
    if (a_unknown(args, al, e)) return NULL;
    JVal *in, *keys;
    if (a_need_obj(args, "input", &in, e)) return NULL;
    if (a_need_arr(args, "keys", &keys, e)) return NULL;
    /* collect requested keys, then emit sorted by key */
    const char **ks = xmalloc((keys->n ? keys->n : 1) * sizeof(char *));
    size_t *kl = xmalloc((keys->n ? keys->n : 1) * sizeof(size_t));
    size_t nk = 0;
    for (size_t i = 0; i < keys->n; i++) {
        if (keys->it[i]->t != J_STR)
            return errv(e, "INVALID_ARG", "pick: key %zu is not a string", i);
        int dup = 0;
        for (size_t j = 0; j < nk; j++) {
            if (kl[j] == keys->it[i]->slen &&
                memcmp(ks[j], keys->it[i]->s, kl[j]) == 0) {
                dup = 1;
                break;
            }
        }
        if (dup) continue;
        ks[nk] = keys->it[i]->s;
        kl[nk] = keys->it[i]->slen;
        nk++;
    }
    /* insertion sort by key */
    for (size_t i = 1; i < nk; i++) {
        const char *k = ks[i];
        size_t l = kl[i];
        size_t j = i;
        while (j > 0 && cmp_bytes(ks[j - 1], kl[j - 1], k, l) > 0) {
            ks[j] = ks[j - 1];
            kl[j] = kl[j - 1];
            j--;
        }
        ks[j] = k;
        kl[j] = l;
    }
    JVal *out = jobj();
    for (size_t i = 0; i < nk; i++) {
        JVal *v = NULL;
        for (size_t j = 0; j < in->mn; j++) {
            if (in->mem[j].klen == kl[i] && memcmp(in->mem[j].key, ks[i], kl[i]) == 0) {
                v = in->mem[j].val;
                break;
            }
        }
        o_put(out, ks[i], kl[i], v ? v : jnull());
    }
    free(ks);
    free(kl);
    return out;
}

static JVal *op_omit(const JVal *args, Err *e) {
    static const char *const al[] = {"input", "keys", NULL};
    if (a_unknown(args, al, e)) return NULL;
    JVal *in, *keys;
    if (a_need_obj(args, "input", &in, e)) return NULL;
    if (a_need_arr(args, "keys", &keys, e)) return NULL;
    for (size_t i = 0; i < keys->n; i++)
        if (keys->it[i]->t != J_STR)
            return errv(e, "INVALID_ARG", "omit: key %zu is not a string", i);
    JVal *out = jobj();
    for (size_t i = 0; i < in->mn; i++) {
        int skip = 0;
        for (size_t j = 0; j < keys->n; j++) {
            if (keys->it[j]->slen == in->mem[i].klen &&
                memcmp(keys->it[j]->s, in->mem[i].key, in->mem[i].klen) == 0) {
                skip = 1;
                break;
            }
        }
        if (!skip) o_put(out, in->mem[i].key, in->mem[i].klen, in->mem[i].val);
    }
    return out;
}

static JVal *op_constant(const JVal *args, Err *e) {
    static const char *const al[] = {"value", NULL};
    if (a_unknown(args, al, e)) return NULL;
    JVal *v = NULL;
    if (a_need_val(args, "value", &v, e)) return NULL;
    return v;
}

static JVal *op_equals(const JVal *args, Err *e) {
    static const char *const al[] = {"a", "b", NULL};
    if (a_unknown(args, al, e)) return NULL;
    JVal *a = NULL, *b = NULL;
    if (a_need_val(args, "a", &a, e)) return NULL;
    if (a_need_val(args, "b", &b, e)) return NULL;
    return jbool(jequal(a, b));
}

static JVal *op_compare(const JVal *args, Err *e) {
    static const char *const al[] = {"a", "b", "op", NULL};
    if (a_unknown(args, al, e)) return NULL;
    JVal *a = NULL, *b = NULL;
    const char *opn;
    size_t opnl;
    if (a_need_val(args, "a", &a, e)) return NULL;
    if (a_need_val(args, "b", &b, e)) return NULL;
    if (a_need_str(args, "op", &opn, &opnl, e)) return NULL;
    int c = compare_values(a, b);
    if (opnl == 2 && !memcmp(opn, "gt", 2)) return jbool(c > 0);
    if (opnl == 3 && !memcmp(opn, "gte", 3)) return jbool(c >= 0);
    if (opnl == 2 && !memcmp(opn, "lt", 2)) return jbool(c < 0);
    if (opnl == 3 && !memcmp(opn, "lte", 3)) return jbool(c <= 0);
    return errv(e, "INVALID_ARG", "compare: unsupported op '%.*s'", (int)opnl, opn);
}

static JVal *op_and(const JVal *args, Err *e) {
    static const char *const al[] = {"a", "b", NULL};
    if (a_unknown(args, al, e)) return NULL;
    JVal *a = NULL, *b = NULL;
    if (a_need_val(args, "a", &a, e)) return NULL;
    if (a_need_val(args, "b", &b, e)) return NULL;
    return jbool(truthy(a) && truthy(b));
}

static JVal *op_or(const JVal *args, Err *e) {
    static const char *const al[] = {"a", "b", NULL};
    if (a_unknown(args, al, e)) return NULL;
    JVal *a = NULL, *b = NULL;
    if (a_need_val(args, "a", &a, e)) return NULL;
    if (a_need_val(args, "b", &b, e)) return NULL;
    return jbool(truthy(a) || truthy(b));
}

static JVal *op_not(const JVal *args, Err *e) {
    static const char *const al[] = {"a", NULL};
    if (a_unknown(args, al, e)) return NULL;
    JVal *a = NULL;
    if (a_need_val(args, "a", &a, e)) return NULL;
    return jbool(!truthy(a));
}

static JVal *op_type(const JVal *args, Err *e) {
    static const char *const al[] = {"value", NULL};
    if (a_unknown(args, al, e)) return NULL;
    JVal *v = NULL;
    if (a_need_val(args, "value", &v, e)) return NULL;
    return jstr(type_name(v), strlen(type_name(v)));
}

static JVal *op_fail_until(const JVal *args, int64_t *counter, Err *e) {
    static const char *const al[] = {"failures", "value", NULL};
    if (a_unknown(args, al, e)) return NULL;
    int64_t failures = 0;
    JVal *val = NULL;
    if (a_need_i64(args, "failures", &failures, e)) return NULL;
    if (failures < 0 || failures > 10)
        return errv(e, "INVALID_ARG", "fail_until: failures must be in 0..10");
    if (a_need_val(args, "value", &val, e)) return NULL;
    (*counter)++;
    if (*counter <= failures)
        return errv(e, "TRANSIENT_FAILURE", "transient failure (attempt %" PRId64 ")",
                    *counter);
    return val;
}

static const char *const OP_NAMES[] = {
    "add", "subtract", "multiply", "divide", "mod", "sum", "min", "max", "range",
    "concat", "split", "join", "uppercase", "lowercase", "trim", "replace", "length",
    "map", "filter", "reduce", "sort", "reverse", "unique", "slice", "flatten",
    "get", "set", "keys", "values", "merge", "pick", "omit",
    "constant", "equals", "compare", "and", "or", "not", "type",
    "fail_until",
    NULL
};

static int is_known_op(const char *op) {
    for (const char *const *p = OP_NAMES; *p; p++)
        if (strcmp(*p, op) == 0) return 1;
    return 0;
}

static JVal *run_op(const char *op, const JVal *args, int64_t *failctr, Err *e) {
    if (!strcmp(op, "add") || !strcmp(op, "subtract") || !strcmp(op, "multiply") ||
        !strcmp(op, "divide") || !strcmp(op, "mod"))
        return op_bin2(op, args, e);
    if (!strcmp(op, "sum")) return op_sum(args, e);
    if (!strcmp(op, "min")) return op_minmax("min", args, e);
    if (!strcmp(op, "max")) return op_minmax("max", args, e);
    if (!strcmp(op, "range")) return op_range(args, e);
    if (!strcmp(op, "concat")) return op_concat(args, e);
    if (!strcmp(op, "split")) return op_split(args, e);
    if (!strcmp(op, "join")) return op_join(args, e);
    if (!strcmp(op, "uppercase")) return op_case("upper", args, e);
    if (!strcmp(op, "lowercase")) return op_case("lower", args, e);
    if (!strcmp(op, "trim")) return op_trim(args, e);
    if (!strcmp(op, "replace")) return op_replace(args, e);
    if (!strcmp(op, "length")) return op_length(args, e);
    if (!strcmp(op, "map")) return op_map(args, e);
    if (!strcmp(op, "filter")) return op_filter(args, e);
    if (!strcmp(op, "reduce")) return op_reduce(args, e);
    if (!strcmp(op, "sort")) return op_sort(args, e);
    if (!strcmp(op, "reverse")) return op_reverse(args, e);
    if (!strcmp(op, "unique")) return op_unique(args, e);
    if (!strcmp(op, "slice")) return op_slice(args, e);
    if (!strcmp(op, "flatten")) return op_flatten(args, e);
    if (!strcmp(op, "get")) return op_get(args, e);
    if (!strcmp(op, "set")) return op_set(args, e);
    if (!strcmp(op, "keys")) return op_keys(args, e);
    if (!strcmp(op, "values")) return op_values(args, e);
    if (!strcmp(op, "merge")) return op_merge(args, e);
    if (!strcmp(op, "pick")) return op_pick(args, e);
    if (!strcmp(op, "omit")) return op_omit(args, e);
    if (!strcmp(op, "constant")) return op_constant(args, e);
    if (!strcmp(op, "equals")) return op_equals(args, e);
    if (!strcmp(op, "compare")) return op_compare(args, e);
    if (!strcmp(op, "and")) return op_and(args, e);
    if (!strcmp(op, "or")) return op_or(args, e);
    if (!strcmp(op, "not")) return op_not(args, e);
    if (!strcmp(op, "type")) return op_type(args, e);
    if (!strcmp(op, "fail_until")) return op_fail_until(args, failctr, e);
    return errv(e, "INVALID_WORKFLOW", "unknown op '%s'", op);
}

/* ------------------------------------------------------------------ */
/* Workflow model                                                      */
/* ------------------------------------------------------------------ */

enum { ST_PENDING = 0, ST_DONE = 1, ST_SKIPPED = 2, ST_FAILED = 3 };

typedef struct {
    char       *id;
    size_t      idlen;
    char       *op;
    JVal       *args;
    JVal       *when;
    int         max_attempts;

    char      **dep_ids;
    size_t     *dep_idlen;
    int        *deps;
    int         ndeps;

    int        *anc;
    int         nanc;

    int         state;
    JVal       *result;
    int64_t     fail_attempts;
} Step;

typedef struct {
    Step *steps;
    int   n;
    int   cur;
} ResCtx;

static int find_step(const Step *steps, int n, const char *id, size_t len) {
    for (int i = 0; i < n; i++)
        if (steps[i].idlen == len && memcmp(steps[i].id, id, len) == 0)
            return i;
    return -1;
}

static int is_ancestor(const Step *s, int idx) {
    for (int i = 0; i < s->nanc; i++)
        if (s->anc[i] == idx) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Reference resolution                                                */
/* ------------------------------------------------------------------ */

static int resolve_value(ResCtx *rc, const JVal *v, JVal **out, Err *e);

static int resolve_ref(ResCtx *rc, const JVal *refstr, JVal **out, Err *e) {
    const char *r = refstr->s;
    size_t len = refstr->slen;
    size_t tl = 0;
    while (tl < len && r[tl] != '.') tl++;
    if (tl == 0)
        return seterr(e, "INVALID_REFERENCE", "empty reference target");
    int idx = find_step(rc->steps, rc->n, r, tl);
    if (idx < 0)
        return seterr(e, "INVALID_REFERENCE", "unknown reference target '%.*s'",
                      (int)tl, r);
    if (idx == rc->cur)
        return seterr(e, "INVALID_REFERENCE", "self reference");
    if (!is_ancestor(&rc->steps[rc->cur], idx))
        return seterr(e, "INVALID_REFERENCE", "reference to non-dependency '%.*s'",
                      (int)tl, r);
    if (rc->steps[idx].state == ST_SKIPPED)
        return seterr(e, "SKIPPED_REFERENCE", "reference to skipped step '%.*s'",
                      (int)tl, r);
    if (rc->steps[idx].state != ST_DONE)
        return seterr(e, "INVALID_REFERENCE", "reference to unexecuted step '%.*s'",
                      (int)tl, r);

    JVal *cur = rc->steps[idx].result;
    size_t pos = tl;
    while (pos < len) {
        /* r[pos] == '.' */
        pos++;
        size_t start = pos;
        while (pos < len && r[pos] != '.') pos++;
        size_t seglen = pos - start;
        if (seglen == 0)
            return seterr(e, "INVALID_REFERENCE", "empty path segment in '%.*s'",
                          (int)len, r);
        if (cur->t == J_OBJ) {
            JVal *found = NULL;
            for (size_t i = 0; i < cur->mn; i++) {
                if (cur->mem[i].klen == seglen &&
                    memcmp(cur->mem[i].key, r + start, seglen) == 0) {
                    found = cur->mem[i].val;
                    break;
                }
            }
            if (!found)
                return seterr(e, "INVALID_REFERENCE", "missing key '%.*s' in '%.*s'",
                              (int)seglen, r + start, (int)len, r);
            cur = found;
        } else if (cur->t == J_ARR) {
            uint64_t idxi = 0;
            for (size_t k = 0; k < seglen; k++) {
                char c = r[start + k];
                if (!is_digit(c))
                    return seterr(e, "INVALID_REFERENCE",
                                  "bad array index '%.*s' in '%.*s'",
                                  (int)seglen, r + start, (int)len, r);
                if (idxi <= 1000000000000ull) idxi = idxi * 10 + (uint64_t)(c - '0');
            }
            if (idxi >= (uint64_t)cur->n)
                return seterr(e, "INVALID_REFERENCE", "index %" PRIu64 " out of range in '%.*s'",
                              idxi, (int)len, r);
            cur = cur->it[(size_t)idxi];
        } else {
            return seterr(e, "INVALID_REFERENCE",
                          "cannot descend into non-container in '%.*s'", (int)len, r);
        }
    }
    *out = cur;
    return 0;
}

static int resolve_value(ResCtx *rc, const JVal *v, JVal **out, Err *e) {
    if (v->t == J_OBJ) {
        if (v->mn == 1 && v->mem[0].klen == 4 &&
            memcmp(v->mem[0].key, "$ref", 4) == 0 &&
            v->mem[0].val->t == J_STR)
            return resolve_ref(rc, v->mem[0].val, out, e);
        JVal *o = jobj();
        for (size_t i = 0; i < v->mn; i++) {
            JVal *nv;
            if (resolve_value(rc, v->mem[i].val, &nv, e)) return -1;
            o_append(o, v->mem[i].key, v->mem[i].klen, nv);
        }
        *out = o;
        return 0;
    }
    if (v->t == J_ARR) {
        JVal *a = jarr();
        for (size_t i = 0; i < v->n; i++) {
            JVal *nv;
            if (resolve_value(rc, v->it[i], &nv, e)) return -1;
            a_push(a, nv);
        }
        *out = a;
        return 0;
    }
    *out = (JVal *)v;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Conditions                                                          */
/* ------------------------------------------------------------------ */

static int eval_cond(ResCtx *rc, const JVal *c, int *out, Err *e) {
    if (!c || c->t != J_OBJ)
        return seterr(e, "INVALID_CONDITION", "condition must be an object");
    JVal *opv = o_get(c, "op");
    if (!opv || opv->t != J_STR)
        return seterr(e, "INVALID_CONDITION", "condition requires string 'op'");
    const char *op = opv->s;

    if (!strcmp(op, "all") || !strcmp(op, "any")) {
        JVal *conds = o_get(c, "conditions");
        if (!conds || conds->t != J_ARR)
            return seterr(e, "INVALID_CONDITION", "'%s' requires array 'conditions'", op);
        int is_all = !strcmp(op, "all");
        int res = is_all ? 1 : 0;
        for (size_t i = 0; i < conds->n; i++) {
            int v;
            if (eval_cond(rc, conds->it[i], &v, e)) return -1;
            if (is_all) {
                if (!v) {
                    res = 0;
                    break;
                }
            } else {
                if (v) {
                    res = 1;
                    break;
                }
            }
        }
        *out = res;
        return 0;
    }
    if (!strcmp(op, "not")) {
        JVal *cond = o_get(c, "condition");
        if (!cond)
            return seterr(e, "INVALID_CONDITION", "'not' requires 'condition'");
        int v;
        if (eval_cond(rc, cond, &v, e)) return -1;
        *out = !v;
        return 0;
    }
    if (!strcmp(op, "eq") || !strcmp(op, "ne") || !strcmp(op, "gt") ||
        !strcmp(op, "gte") || !strcmp(op, "lt") || !strcmp(op, "lte")) {
        JVal *lv = o_get(c, "left");
        JVal *rv = o_get(c, "right");
        if (!lv || !rv)
            return seterr(e, "INVALID_CONDITION", "condition requires 'left' and 'right'");
        JVal *l, *r;
        if (resolve_value(rc, lv, &l, e)) return -1;
        if (resolve_value(rc, rv, &r, e)) return -1;
        if (!strcmp(op, "eq")) {
            *out = jequal(l, r);
        } else if (!strcmp(op, "ne")) {
            *out = !jequal(l, r);
        } else {
            int cmp = compare_values(l, r);
            if (!strcmp(op, "gt")) *out = cmp > 0;
            else if (!strcmp(op, "gte")) *out = cmp >= 0;
            else if (!strcmp(op, "lt")) *out = cmp < 0;
            else *out = cmp <= 0;
        }
        return 0;
    }
    return seterr(e, "INVALID_CONDITION", "unknown condition op '%s'", op);
}

/* ------------------------------------------------------------------ */
/* Structural validation                                               */
/* ------------------------------------------------------------------ */

static int valid_ident(const char *s, size_t n) {
    if (n == 0) return 0;
    char c = s[0];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) return 0;
    for (size_t i = 1; i < n; i++) {
        c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_'))
            return 0;
    }
    return 1;
}

static int load_workflow(JVal *doc, Step **out_steps, int *out_n, Err *e) {
    static const char *const top_allowed[] = {"version", "steps", NULL};
    static const char *const step_allowed[] = {
        "id", "op", "args", "depends_on", "when", "retry", NULL
    };

    if (!doc || doc->t != J_OBJ)
        return seterr(e, "INVALID_WORKFLOW", "workflow document must be an object");

    for (size_t i = 0; i < doc->mn; i++) {
        int ok = 0;
        for (const char *const *p = top_allowed; *p; p++) {
            if (strlen(*p) == doc->mem[i].klen &&
                memcmp(*p, doc->mem[i].key, doc->mem[i].klen) == 0) {
                ok = 1;
                break;
            }
        }
        if (!ok)
            return seterr(e, "INVALID_WORKFLOW", "unknown top-level field '%.*s'",
                          (int)doc->mem[i].klen, doc->mem[i].key);
    }

    JVal *ver = o_get(doc, "version");
    if (!ver)
        return seterr(e, "INVALID_WORKFLOW", "missing 'version'");
    if (!is_i64(ver) || ver->i != 1)
        return seterr(e, "INVALID_WORKFLOW", "version must be the integer 1");

    JVal *stepsv = o_get(doc, "steps");
    if (!stepsv)
        return seterr(e, "INVALID_WORKFLOW", "missing 'steps'");
    if (stepsv->t != J_ARR)
        return seterr(e, "INVALID_WORKFLOW", "'steps' must be an array");

    int n = (int)stepsv->n;
    Step *steps = xcalloc((size_t)(n ? n : 1), sizeof(Step));

    for (int i = 0; i < n; i++) {
        JVal *s = stepsv->it[i];
        e->step = NULL;
        if (!s || s->t != J_OBJ)
            return seterr(e, "INVALID_WORKFLOW", "step %d is not an object", i);

        JVal *idv = o_get(s, "id");
        /* Determine the step's id for error reporting (even if malformed). */
        if (idv && idv->t == J_STR) {
            steps[i].id = xstrndup(idv->s, idv->slen);
            steps[i].idlen = idv->slen;
            e->step = steps[i].id;
        } else {
            e->step = NULL;
        }

        for (size_t k = 0; k < s->mn; k++) {
            int ok = 0;
            for (const char *const *p = step_allowed; *p; p++) {
                if (strlen(*p) == s->mem[k].klen &&
                    memcmp(*p, s->mem[k].key, s->mem[k].klen) == 0) {
                    ok = 1;
                    break;
                }
            }
            if (!ok)
                return seterr(e, "INVALID_WORKFLOW", "unknown step field '%.*s'",
                              (int)s->mem[k].klen, s->mem[k].key);
        }

        if (!idv)
            return seterr(e, "INVALID_WORKFLOW", "step is missing 'id'");
        if (idv->t != J_STR)
            return seterr(e, "INVALID_WORKFLOW", "step 'id' must be a string");
        if (!valid_ident(steps[i].id, steps[i].idlen))
            return seterr(e, "INVALID_WORKFLOW", "invalid step id '%s'", steps[i].id);
        for (int j = 0; j < i; j++) {
            if (steps[j].idlen == steps[i].idlen &&
                memcmp(steps[j].id, steps[i].id, steps[i].idlen) == 0)
                return seterr(e, "INVALID_WORKFLOW", "duplicate step id '%s'",
                              steps[i].id);
        }

        JVal *opv = o_get(s, "op");
        if (!opv)
            return seterr(e, "INVALID_WORKFLOW", "step '%s' is missing 'op'",
                          steps[i].id);
        if (opv->t != J_STR)
            return seterr(e, "INVALID_WORKFLOW", "step '%s': 'op' must be a string",
                          steps[i].id);
        if (!is_known_op(opv->s))
            return seterr(e, "INVALID_WORKFLOW", "step '%s': unknown op '%s'",
                          steps[i].id, opv->s);
        steps[i].op = xstrndup(opv->s, opv->slen);

        JVal *argsv = o_get(s, "args");
        if (argsv) {
            if (argsv->t != J_OBJ)
                return seterr(e, "INVALID_WORKFLOW", "step '%s': 'args' must be an object",
                              steps[i].id);
            steps[i].args = argsv;
        } else {
            steps[i].args = jobj();
        }

        JVal *whenv = o_get(s, "when");
        if (whenv) {
            if (whenv->t != J_OBJ)
                return seterr(e, "INVALID_WORKFLOW", "step '%s': 'when' must be an object",
                              steps[i].id);
            steps[i].when = whenv;
        }

        JVal *retryv = o_get(s, "retry");
        steps[i].max_attempts = 1;
        if (retryv) {
            if (retryv->t != J_OBJ)
                return seterr(e, "INVALID_WORKFLOW", "step '%s': 'retry' must be an object",
                              steps[i].id);
            for (size_t k = 0; k < retryv->mn; k++) {
                if (!(retryv->mem[k].klen == 12 &&
                      memcmp(retryv->mem[k].key, "max_attempts", 12) == 0))
                    return seterr(e, "INVALID_WORKFLOW",
                                  "step '%s': unknown retry field '%.*s'",
                                  steps[i].id, (int)retryv->mem[k].klen,
                                  retryv->mem[k].key);
            }
            JVal *mav = o_get(retryv, "max_attempts");
            if (mav) {
                if (!is_i64(mav) || mav->i < 1 || mav->i > 5)
                    return seterr(e, "INVALID_WORKFLOW",
                                  "step '%s': max_attempts must be 1..5", steps[i].id);
                steps[i].max_attempts = (int)mav->i;
            }
        }

        JVal *depsv = o_get(s, "depends_on");
        if (depsv) {
            if (depsv->t != J_ARR)
                return seterr(e, "INVALID_WORKFLOW",
                              "step '%s': 'depends_on' must be an array", steps[i].id);
            steps[i].ndeps = (int)depsv->n;
            steps[i].dep_ids = xmalloc((size_t)(depsv->n ? depsv->n : 1) * sizeof(char *));
            steps[i].dep_idlen = xmalloc((size_t)(depsv->n ? depsv->n : 1) * sizeof(size_t));
            for (size_t k = 0; k < depsv->n; k++) {
                if (depsv->it[k]->t != J_STR)
                    return seterr(e, "INVALID_WORKFLOW",
                                  "step '%s': depends_on entries must be strings",
                                  steps[i].id);
                steps[i].dep_ids[k] = depsv->it[k]->s;
                steps[i].dep_idlen[k] = depsv->it[k]->slen;
            }
        }
    }

    /* Deps exist? */
    for (int i = 0; i < n; i++) {
        steps[i].deps = xmalloc((size_t)(steps[i].ndeps ? steps[i].ndeps : 1) * sizeof(int));
        for (int k = 0; k < steps[i].ndeps; k++) {
            int d = find_step(steps, n, steps[i].dep_ids[k], steps[i].dep_idlen[k]);
            if (d < 0) {
                e->step = steps[i].id;
                return seterr(e, "UNKNOWN_DEPENDENCY", "step '%s' depends on unknown '%s'",
                              steps[i].id, steps[i].dep_ids[k]);
            }
            steps[i].deps[k] = d;
        }
        steps[i].dep_ids = NULL;
        steps[i].dep_idlen = NULL;
    }

    /* Cycle detection (Kahn-style, O(n^2)). */
    {
        char *done = xcalloc((size_t)(n ? n : 1), 1);
        int cnt = 0;
        int progress = 1;
        while (progress) {
            progress = 0;
            for (int i = 0; i < n; i++) {
                if (done[i]) continue;
                int ready = 1;
                for (int k = 0; k < steps[i].ndeps; k++) {
                    if (!done[steps[i].deps[k]]) {
                        ready = 0;
                        break;
                    }
                }
                if (ready) {
                    done[i] = 1;
                    cnt++;
                    progress = 1;
                }
            }
        }
        free(done);
        if (cnt < n) {
            e->step = NULL;
            return seterr(e, "CYCLIC_DEPENDENCY", "dependency cycle detected");
        }
    }

    /* Ancestors. */
    for (int i = 0; i < n; i++) {
        char *seen = xcalloc((size_t)(n ? n : 1), 1);
        /* iterative DFS */
        int *stack = xmalloc((size_t)(n ? n : 1) * sizeof(int));
        int sp = 0;
        stack[sp++] = i;
        seen[i] = 1;
        int count = 0;
        while (sp > 0) {
            int cur = stack[--sp];
            for (int k = 0; k < steps[cur].ndeps; k++) {
                int d = steps[cur].deps[k];
                if (!seen[d]) {
                    seen[d] = 1;
                    count++;
                    stack[sp++] = d;
                }
            }
        }
        steps[i].anc = xmalloc((size_t)(count ? count : 1) * sizeof(int));
        steps[i].nanc = 0;
        for (int k = 0; k < n; k++)
            if (seen[k] && k != i) steps[i].anc[steps[i].nanc++] = k;
        free(stack);
        free(seen);
    }

    *out_steps = steps;
    *out_n = n;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Execution                                                           */
/* ------------------------------------------------------------------ */

static void w_id_list(Buf *out, Step *steps, const int *list, int n) {
    bputc(out, '[');
    for (int i = 0; i < n; i++) {
        if (i) bputc(out, ',');
        wstr(out, steps[list[i]].id, steps[list[i]].idlen);
    }
    bputc(out, ']');
}

static int exec_workflow(Step *steps, int n, Buf *out) {
    int *order = xmalloc((size_t)(n ? n : 1) * sizeof(int));
    int norder = 0;
    int *skipped = xmalloc((size_t)(n ? n : 1) * sizeof(int));
    int nskipped = 0;

    Err e;
    memset(&e, 0, sizeof e);
    int failed = 0;
    const char *failstep = NULL;
    int remaining = n;

    while (remaining > 0) {
        int pick = -1;
        for (int i = 0; i < n; i++) {
            if (steps[i].state != ST_PENDING) continue;
            int ready = 1;
            for (int k = 0; k < steps[i].ndeps; k++) {
                int st = steps[steps[i].deps[k]].state;
                if (st != ST_DONE && st != ST_SKIPPED) {
                    ready = 0;
                    break;
                }
            }
            if (ready) {
                pick = i;
                break;
            }
        }
        if (pick < 0) {
            seterr(&e, "INVALID_WORKFLOW", "no runnable step found");
            failed = 1;
            failstep = NULL;
            break;
        }

        Step *s = &steps[pick];
        ResCtx rc = { steps, n, pick };

        if (s->when) {
            int cond = 0;
            if (eval_cond(&rc, s->when, &cond, &e)) {
                failed = 1;
                failstep = s->id;
                s->state = ST_FAILED;
                break;
            }
            if (!cond) {
                s->state = ST_SKIPPED;
                s->result = jnull();
                order[norder++] = pick;
                skipped[nskipped++] = pick;
                remaining--;
                continue;
            }
        }

        JVal *resolved = NULL;
        if (resolve_value(&rc, s->args, &resolved, &e)) {
            failed = 1;
            failstep = s->id;
            s->state = ST_FAILED;
            break;
        }

        JVal *res = NULL;
        int attempt = 0;
        for (;;) {
            attempt++;
            Err ee;
            memset(&ee, 0, sizeof ee);
            JVal *v = run_op(s->op, resolved, &s->fail_attempts, &ee);
            if (v) {
                res = v;
                break;
            }
            if (strcmp(ee.code, "TRANSIENT_FAILURE") == 0) {
                if (attempt < s->max_attempts) continue;
                seterr(&e, "STEP_FAILED", "%s", ee.msg);
            } else {
                e = ee;
            }
            break;
        }
        if (!res) {
            failed = 1;
            failstep = s->id;
            s->state = ST_FAILED;
            break;
        }
        s->state = ST_DONE;
        s->result = res;
        order[norder++] = pick;
        remaining--;
    }

    bputs(out, "{\"status\":\"");
    bputs(out, failed ? "failed" : "completed");
    bputs(out, "\",\"results\":{");
    int first = 1;
    for (int i = 0; i < n; i++) {
        if (steps[i].state != ST_DONE && steps[i].state != ST_SKIPPED) continue;
        if (!first) bputc(out, ',');
        first = 0;
        wstr(out, steps[i].id, steps[i].idlen);
        bputc(out, ':');
        if (steps[i].state == ST_SKIPPED) bputs(out, "null");
        else wjson(out, steps[i].result);
    }
    bputs(out, "},\"execution_order\":");
    w_id_list(out, steps, order, norder);
    bputs(out, ",\"skipped\":");
    w_id_list(out, steps, skipped, nskipped);
    bputs(out, ",\"error\":");
    if (failed) {
        bputs(out, "{\"code\":");
        wstr(out, e.code, strlen(e.code));
        bputs(out, ",\"step\":");
        if (failstep) wstr(out, failstep, strlen(failstep));
        else bputs(out, "null");
        bputs(out, ",\"message\":");
        wstr(out, e.msg, strlen(e.msg));
        bputc(out, '}');
    } else {
        bputs(out, "null");
    }
    bputc(out, '}');

    free(order);
    free(skipped);
    return failed ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

static void emit_error(Buf *out, const char *code, const char *step, const char *msg) {
    bputs(out, "{\"status\":\"failed\",\"results\":{},\"execution_order\":[],"
               "\"skipped\":[],\"error\":{\"code\":");
    wstr(out, code, strlen(code));
    bputs(out, ",\"step\":");
    if (step) wstr(out, step, strlen(step));
    else bputs(out, "null");
    bputs(out, ",\"message\":");
    wstr(out, msg, strlen(msg));
    bputs(out, "}}");
}

static int read_all(const char *path, char **out, size_t *outlen) {
    FILE *f = NULL;
    int is_stdin = (strcmp(path, "-") == 0);
    if (is_stdin) {
        f = stdin;
    } else {
        f = fopen(path, "rb");
        if (!f) return -1;
    }
    size_t cap = 65536, n = 0;
    char *buf = xmalloc(cap);
    for (;;) {
        if (n == cap) {
            cap *= 2;
            buf = xrealloc(buf, cap);
        }
        size_t got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (got == 0) {
            if (ferror(f)) {
                if (!is_stdin) fclose(f);
                free(buf);
                return -1;
            }
            break;
        }
    }
    if (!is_stdin) fclose(f);
    *out = buf;
    *outlen = n;
    return 0;
}

int main(int argc, char **argv) {
    Buf out;
    memset(&out, 0, sizeof out);

    if (argc != 3 || strcmp(argv[1], "run") != 0) {
        emit_error(&out, "INVALID_WORKFLOW", NULL,
                   "usage: workflow run <workflow.json|->");
        fwrite(out.p, 1, out.n, stdout);
        fputc('\n', stdout);
        return 2;
    }

    char *data = NULL;
    size_t len = 0;
    if (read_all(argv[2], &data, &len) != 0) {
        emit_error(&out, "INVALID_WORKFLOW", NULL, "cannot read input document");
        fwrite(out.p, 1, out.n, stdout);
        fputc('\n', stdout);
        return 2;
    }

    Parser ps;
    memset(&ps, 0, sizeof ps);
    ps.p = data;
    ps.end = data + len;

    JVal *doc = p_value(&ps);
    if (!doc) {
        char msg[200];
        snprintf(msg, sizeof msg, "invalid JSON: %s", ps.err);
        emit_error(&out, "INVALID_WORKFLOW", NULL, msg);
        fwrite(out.p, 1, out.n, stdout);
        fputc('\n', stdout);
        return 2;
    }
    skip_ws(&ps);
    if (ps.p != ps.end) {
        emit_error(&out, "INVALID_WORKFLOW", NULL, "trailing content after JSON document");
        fwrite(out.p, 1, out.n, stdout);
        fputc('\n', stdout);
        return 2;
    }

    Step *steps = NULL;
    int n = 0;
    Err e;
    memset(&e, 0, sizeof e);

    if (load_workflow(doc, &steps, &n, &e) != 0) {
        emit_error(&out, e.code, e.step, e.msg);
        fwrite(out.p, 1, out.n, stdout);
        fputc('\n', stdout);
        int code = 1;
        if (!strcmp(e.code, "INVALID_WORKFLOW") ||
            !strcmp(e.code, "UNKNOWN_DEPENDENCY") ||
            !strcmp(e.code, "CYCLIC_DEPENDENCY"))
            code = 2;
        return code;
    }

    int rc = exec_workflow(steps, n, &out);
    fwrite(out.p, 1, out.n, stdout);
    fputc('\n', stdout);
    return rc;
}
