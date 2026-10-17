/*
 * expr - a small typed expression language evaluator.
 *
 * Implements the contract in tasks/expr/SPEC.md:
 *   expr eval <request.json>
 *   expr eval -        (request on stdin)
 *
 * Standard C library only.  A hand written JSON reader/writer and a hand
 * written lexer / recursive descent parser / evaluator for the expression
 * language.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

/* ------------------------------------------------------------------ */
/* small utilities                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *p;
    size_t n;
    size_t cap;
} Buf;

static void buf_init(Buf *b) { b->p = NULL; b->n = 0; b->cap = 0; }

static void buf_reserve(Buf *b, size_t need)
{
    if (b->n + need <= b->cap)
        return;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->n + need) {
        if (cap > ((size_t)-1) / 2) {
            fprintf(stderr, "expr: out of memory\n");
            exit(3);
        }
        cap *= 2;
    }
    char *q = realloc(b->p, cap);
    if (!q) {
        fprintf(stderr, "expr: out of memory\n");
        exit(3);
    }
    b->p = q;
    b->cap = cap;
}

static void buf_add(Buf *b, const void *d, size_t n)
{
    if (n == 0)
        return;
    buf_reserve(b, n);
    memcpy(b->p + b->n, d, n);
    b->n += n;
}

static void buf_addc(Buf *b, char c)
{
    buf_reserve(b, 1);
    b->p[b->n++] = c;
}

static void buf_adds(Buf *b, const char *s)
{
    buf_add(b, s, strlen(s));
}

static void buf_free(Buf *b)
{
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

/* arena allocator: everything lives until the request is answered */
typedef struct ArenaBlock ArenaBlock;
struct ArenaBlock {
    ArenaBlock *next;
    size_t      used;
    size_t      cap;
    char        data[1];
};

typedef struct {
    ArenaBlock *head;
} Arena;

static void *arena_alloc(Arena *a, size_t n)
{
    n = (n + 15u) & ~(size_t)15u;
    if (n == 0)
        n = 16;
    if (!a->head || a->head->used + n > a->head->cap) {
        size_t cap = n > 65536 ? n : 65536;
        ArenaBlock *b = malloc(sizeof(ArenaBlock) + cap);
        if (!b) {
            fprintf(stderr, "expr: out of memory\n");
            exit(3);
        }
        b->used = 0;
        b->cap = cap;
        b->next = a->head;
        a->head = b;
    }
    void *p = a->head->data + a->head->used;
    a->head->used += n;
    return p;
}

static void arena_free(Arena *a)
{
    ArenaBlock *b = a->head;
    while (b) {
        ArenaBlock *n = b->next;
        free(b);
        b = n;
    }
    a->head = NULL;
}

/* ------------------------------------------------------------------ */
/* errors                                                              */
/* ------------------------------------------------------------------ */

enum {
    ERR_NONE = 0,
    ERR_INVALID_REQUEST,
    ERR_PARSE,
    ERR_OVERFLOW,
    ERR_DIVISION_BY_ZERO,
    ERR_TYPE,
    ERR_UNKNOWN_VARIABLE,
    ERR_UNKNOWN_FUNCTION,
    ERR_ARITY,
    ERR_EMPTY_ARRAY,
    ERR_INTERNAL
};

static const char *err_code_name(int c)
{
    switch (c) {
    case ERR_INVALID_REQUEST:  return "INVALID_REQUEST";
    case ERR_PARSE:            return "PARSE_ERROR";
    case ERR_OVERFLOW:         return "OVERFLOW";
    case ERR_DIVISION_BY_ZERO: return "DIVISION_BY_ZERO";
    case ERR_TYPE:             return "TYPE_ERROR";
    case ERR_UNKNOWN_VARIABLE: return "UNKNOWN_VARIABLE";
    case ERR_UNKNOWN_FUNCTION: return "UNKNOWN_FUNCTION";
    case ERR_ARITY:            return "ARITY_ERROR";
    case ERR_EMPTY_ARRAY:      return "EMPTY_ARRAY";
    default:                   return "INTERNAL_ERROR";
    }
}

static const char *err_message(int c)
{
    switch (c) {
    case ERR_INVALID_REQUEST:  return "invalid request";
    case ERR_PARSE:            return "parse error";
    case ERR_OVERFLOW:         return "integer overflow";
    case ERR_DIVISION_BY_ZERO: return "division by zero";
    case ERR_TYPE:             return "type error";
    case ERR_UNKNOWN_VARIABLE: return "unknown variable";
    case ERR_UNKNOWN_FUNCTION: return "unknown function";
    case ERR_ARITY:            return "wrong number of arguments";
    case ERR_EMPTY_ARRAY:      return "empty array";
    default:                   return "internal error";
    }
}

static int err_exit_code(int c)
{
    switch (c) {
    case ERR_INVALID_REQUEST:  return 2;
    case ERR_INTERNAL:         return 3;
    default:                   return 1;
    }
}

/* byte offset of the offending token (SIZE_MAX means "no position") */
typedef struct {
    int    code;
    size_t pos;
} Err;

#define NOPOS ((size_t)-1)

static void set_err(Err *e, int code, size_t pos)
{
    if (e->code != ERR_NONE)
        return;
    e->code = code;
    e->pos = pos;
}

/* ------------------------------------------------------------------ */
/* UTF-8 helpers                                                       */
/* ------------------------------------------------------------------ */

static size_t u8_len(unsigned char c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1; /* invalid lead byte: treat as a single unit */
}

static size_t utf8_cp_count(const char *p, size_t n)
{
    size_t i = 0, c = 0;
    while (i < n) {
        size_t l = u8_len((unsigned char)p[i]);
        if (i + l > n)
            l = n - i;
        i += l;
        c++;
    }
    return c;
}

/* byte offset of code point index cp (clamped to n) */
static size_t utf8_byte_off(const char *p, size_t n, size_t cp)
{
    size_t i = 0, c = 0;
    while (i < n && c < cp) {
        size_t l = u8_len((unsigned char)p[i]);
        if (i + l > n)
            l = n - i;
        i += l;
        c++;
    }
    return i;
}

typedef struct {
    const char *p;
    size_t      n;
} Str;

static Str str_slice_cp(Str s, size_t start_cp, size_t len_cp)
{
    size_t b0 = utf8_byte_off(s.p, s.n, start_cp);
    size_t b1 = utf8_byte_off(s.p, s.n, start_cp + len_cp);
    Str r;
    r.p = s.p + b0;
    r.n = b1 - b0;
    return r;
}

static void utf8_encode(Buf *b, uint32_t cp)
{
    if (cp > 0x10FFFF)
        cp = 0xFFFD;
    if (cp >= 0xD800 && cp <= 0xDFFF)
        cp = 0xFFFD;
    if (cp < 0x80) {
        buf_addc(b, (char)cp);
    } else if (cp < 0x800) {
        buf_addc(b, (char)(0xC0 | (cp >> 6)));
        buf_addc(b, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        buf_addc(b, (char)(0xE0 | (cp >> 12)));
        buf_addc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        buf_addc(b, (char)(0x80 | (cp & 0x3F)));
    } else {
        buf_addc(b, (char)(0xF0 | (cp >> 18)));
        buf_addc(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
        buf_addc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        buf_addc(b, (char)(0x80 | (cp & 0x3F)));
    }
}

static int hex_val(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex4(const char *p, uint32_t *out)
{
    uint32_t v = 0;
    int i;
    for (i = 0; i < 4; i++) {
        int h = hex_val((unsigned char)p[i]);
        if (h < 0)
            return 0;
        v = (v << 4) | (uint32_t)h;
    }
    *out = v;
    return 1;
}

/*
 * Decode a quoted string body.  src[start] is the first character after the
 * opening quote.  On success, *end_out is the index just past the closing
 * quote and *out is an arena allocated byte string.
 * Returns 0 success, -1 bad escape, -2 unterminated.
 */
static int decode_string(Arena *a, const char *src, size_t n, size_t start,
                         int is_json, size_t *end_out, Str *out)
{
    Buf b;
    size_t i = start;

    buf_init(&b);
    for (;;) {
        unsigned char c;
        if (i >= n) {
            buf_free(&b);
            return -2;
        }
        c = (unsigned char)src[i];
        if (c == '"') {
            i++;
            break;
        }
        if (c == '\\') {
            char e;
            if (i + 1 >= n) {
                buf_free(&b);
                return -2;
            }
            e = src[i + 1];
            switch (e) {
            case '"':  buf_addc(&b, '"');  i += 2; break;
            case '\\': buf_addc(&b, '\\'); i += 2; break;
            case '/':  buf_addc(&b, '/');  i += 2; break;
            case 'n':  buf_addc(&b, '\n'); i += 2; break;
            case 't':  buf_addc(&b, '\t'); i += 2; break;
            case 'r':  buf_addc(&b, '\r'); i += 2; break;
            case 'b':  buf_addc(&b, '\b'); i += 2; break;
            case 'f':  buf_addc(&b, '\f'); i += 2; break;
            case 'u': {
                uint32_t cp;
                if (i + 6 > n || !hex4(src + i + 2, &cp)) {
                    buf_free(&b);
                    return -1;
                }
                i += 6;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    /* try to pair with a following low surrogate */
                    if (i + 6 <= n && src[i] == '\\' && src[i + 1] == 'u') {
                        uint32_t lo;
                        if (hex4(src + i + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                            i += 6;
                        } else {
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
                buf_free(&b);
                return -1;
            }
            continue;
        }
        if (is_json && c < 0x20) {
            buf_free(&b);
            return -1;
        }
        buf_addc(&b, (char)c);
        i++;
    }

    out->n = b.n;
    out->p = arena_alloc(a, b.n ? b.n : 1);
    if (b.n)
        memcpy((char *)out->p, b.p, b.n);
    buf_free(&b);
    *end_out = i;
    return 0;
}

/* ------------------------------------------------------------------ */
/* values                                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    VT_NULL,
    VT_BOOL,
    VT_INT,
    VT_STR,
    VT_ARR
} VType;

typedef struct Value Value;
struct Value {
    VType t;
    union {
        int     b;
        int64_t i;
        Str     s;
        struct {
            Value **v;
            size_t  n;
        } arr;
    } u;
};

typedef struct {
    Str    name;
    Value *val;
} Var;

typedef struct {
    Arena      *a;
    const char *src;
    size_t      srclen;
    Var        *vars;
    size_t      nvars;
} Ctx;

static Value *v_new(Ctx *C, VType t)
{
    Value *v = arena_alloc(C->a, sizeof(Value));
    memset(v, 0, sizeof(*v));
    v->t = t;
    return v;
}

static Value *v_null(Ctx *C) { return v_new(C, VT_NULL); }

static Value *v_bool(Ctx *C, int b)
{
    Value *v = v_new(C, VT_BOOL);
    v->u.b = b ? 1 : 0;
    return v;
}

static Value *v_int(Ctx *C, int64_t i)
{
    Value *v = v_new(C, VT_INT);
    v->u.i = i;
    return v;
}

static Value *v_str(Ctx *C, Str s)
{
    Value *v = v_new(C, VT_STR);
    v->u.s = s;
    return v;
}

static Value *v_arr(Ctx *C, Value **items, size_t n)
{
    Value *v = v_new(C, VT_ARR);
    v->u.arr.v = items;
    v->u.arr.n = n;
    return v;
}

static Value *lookup_var(Ctx *C, Str name)
{
    size_t k = C->nvars;
    while (k > 0) {
        k--;
        if (C->vars[k].name.n == name.n &&
            memcmp(C->vars[k].name.p, name.p, name.n) == 0)
            return C->vars[k].val;
    }
    return NULL;
}

/* structural equality */
static int value_equal(const Value *a, const Value *b)
{
    if (a->t != b->t)
        return 0;
    switch (a->t) {
    case VT_NULL:
        return 1;
    case VT_BOOL:
        return (a->u.b != 0) == (b->u.b != 0);
    case VT_INT:
        return a->u.i == b->u.i;
    case VT_STR:
        return a->u.s.n == b->u.s.n &&
               memcmp(a->u.s.p, b->u.s.p, a->u.s.n) == 0;
    case VT_ARR: {
        size_t i;
        if (a->u.arr.n != b->u.arr.n)
            return 0;
        for (i = 0; i < a->u.arr.n; i++)
            if (!value_equal(a->u.arr.v[i], b->u.arr.v[i]))
                return 0;
        return 1;
    }
    }
    return 0;
}

/* byte-wise (UTF-8 order) string compare */
static int str_compare(Str a, Str b)
{
    size_t m = a.n < b.n ? a.n : b.n;
    int c = m ? memcmp(a.p, b.p, m) : 0;
    if (c != 0)
        return c < 0 ? -1 : 1;
    if (a.n == b.n)
        return 0;
    return a.n < b.n ? -1 : 1;
}

/* ------------------------------------------------------------------ */
/* JSON request reader                                                 */
/* ------------------------------------------------------------------ */

typedef enum {
    J_NULL,
    J_BOOL,
    J_INT,
    J_STR,
    J_ARR,
    J_OBJ
} JType;

typedef struct JVal JVal;
struct JVal {
    JType t;
    int   bad;      /* numeric value not representable as i64 */
    int   b;
    int64_t i;
    Str   s;
    JVal **items;   /* array */
    size_t nitems;
    Str   *keys;    /* object */
    JVal **vals;
    size_t nkeys;
};

typedef struct {
    const char *s;
    size_t n;
    size_t i;
    Arena *a;
    int err;
} JP;

static JVal *jv_new(JP *P, JType t)
{
    JVal *v = arena_alloc(P->a, sizeof(JVal));
    memset(v, 0, sizeof(*v));
    v->t = t;
    return v;
}

static void jp_ws(JP *P)
{
    while (P->i < P->n) {
        char c = P->s[P->i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            P->i++;
        else
            break;
    }
}

static int jp_digit(char c) { return c >= '0' && c <= '9'; }

static JVal *jp_value(JP *P, int depth);

static JVal *jp_string(JP *P)
{
    Str s;
    size_t end;
    int r;
    JVal *v;

    if (P->i >= P->n || P->s[P->i] != '"') {
        P->err = 1;
        return NULL;
    }
    r = decode_string(P->a, P->s, P->n, P->i + 1, 1, &end, &s);
    if (r != 0) {
        P->err = 1;
        return NULL;
    }
    P->i = end;
    v = jv_new(P, J_STR);
    v->s = s;
    return v;
}

static JVal *jp_number(JP *P)
{
    size_t st = P->i;
    int neg = 0, frac = 0;
    JVal *v;

    if (P->i < P->n && P->s[P->i] == '-') {
        neg = 1;
        P->i++;
    }
    if (!(P->i < P->n && jp_digit(P->s[P->i]))) {
        P->err = 1;
        return NULL;
    }
    if (P->s[P->i] == '0') {
        P->i++;
        if (P->i < P->n && jp_digit(P->s[P->i])) {
            P->err = 1;
            return NULL;
        }
    } else {
        while (P->i < P->n && jp_digit(P->s[P->i]))
            P->i++;
    }
    if (P->i < P->n && P->s[P->i] == '.') {
        frac = 1;
        P->i++;
        if (!(P->i < P->n && jp_digit(P->s[P->i]))) {
            P->err = 1;
            return NULL;
        }
        while (P->i < P->n && jp_digit(P->s[P->i]))
            P->i++;
    }
    if (P->i < P->n && (P->s[P->i] == 'e' || P->s[P->i] == 'E')) {
        frac = 1;
        P->i++;
        if (P->i < P->n && (P->s[P->i] == '+' || P->s[P->i] == '-'))
            P->i++;
        if (!(P->i < P->n && jp_digit(P->s[P->i]))) {
            P->err = 1;
            return NULL;
        }
        while (P->i < P->n && jp_digit(P->s[P->i]))
            P->i++;
    }

    v = jv_new(P, J_INT);
    v->bad = frac;
    if (frac)
        return v;

    {
        size_t j = st + (size_t)neg;
        uint64_t acc = 0;
        int over = 0;
        for (; j < P->i; j++) {
            unsigned d = (unsigned)(P->s[j] - '0');
            if (acc > (UINT64_MAX - d) / 10u) {
                over = 1;
                break;
            }
            acc = acc * 10u + d;
        }
        if (over) {
            v->bad = 1;
        } else if (!neg) {
            if (acc > (uint64_t)INT64_MAX) {
                v->bad = 1;
            } else {
                v->i = (int64_t)acc;
            }
        } else {
            if (acc > (uint64_t)INT64_MAX + 1u) {
                v->bad = 1;
            } else if (acc == (uint64_t)INT64_MAX + 1u) {
                v->i = INT64_MIN;
            } else {
                v->i = -(int64_t)acc;
            }
        }
    }
    return v;
}

static JVal *jp_value(JP *P, int depth)
{
    char c;

    if (depth > 4096) {
        P->err = 1;
        return NULL;
    }
    jp_ws(P);
    if (P->i >= P->n) {
        P->err = 1;
        return NULL;
    }
    c = P->s[P->i];
    switch (c) {
    case '"':
        return jp_string(P);
    case '{': {
        JVal *v = jv_new(P, J_OBJ);
        size_t cap = 0;
        Str *keys = NULL;
        JVal **vals = NULL;
        P->i++;
        jp_ws(P);
        if (P->i < P->n && P->s[P->i] == '}') {
            P->i++;
            return v;
        }
        for (;;) {
            JVal *k, *val;
            jp_ws(P);
            if (P->i >= P->n || P->s[P->i] != '"') {
                P->err = 1;
                return NULL;
            }
            k = jp_string(P);
            if (!k)
                return NULL;
            jp_ws(P);
            if (P->i >= P->n || P->s[P->i] != ':') {
                P->err = 1;
                return NULL;
            }
            P->i++;
            val = jp_value(P, depth + 1);
            if (!val)
                return NULL;
            if (v->nkeys == cap) {
                size_t ncap = cap ? cap * 2 : 8;
                Str *nk = arena_alloc(P->a, ncap * sizeof(Str));
                JVal **nv = arena_alloc(P->a, ncap * sizeof(JVal *));
                if (cap) {
                    memcpy(nk, keys, v->nkeys * sizeof(Str));
                    memcpy(nv, vals, v->nkeys * sizeof(JVal *));
                }
                keys = nk;
                vals = nv;
                cap = ncap;
            }
            keys[v->nkeys] = k->s;
            vals[v->nkeys] = val;
            v->nkeys++;
            jp_ws(P);
            if (P->i < P->n && P->s[P->i] == ',') {
                P->i++;
                continue;
            }
            if (P->i < P->n && P->s[P->i] == '}') {
                P->i++;
                break;
            }
            P->err = 1;
            return NULL;
        }
        v->keys = keys;
        v->vals = vals;
        return v;
    }
    case '[': {
        JVal *v = jv_new(P, J_ARR);
        size_t cap = 0;
        JVal **items = NULL;
        P->i++;
        jp_ws(P);
        if (P->i < P->n && P->s[P->i] == ']') {
            P->i++;
            return v;
        }
        for (;;) {
            JVal *it = jp_value(P, depth + 1);
            if (!it)
                return NULL;
            if (v->nitems == cap) {
                size_t ncap = cap ? cap * 2 : 8;
                JVal **ni = arena_alloc(P->a, ncap * sizeof(JVal *));
                if (cap)
                    memcpy(ni, items, v->nitems * sizeof(JVal *));
                items = ni;
                cap = ncap;
            }
            items[v->nitems++] = it;
            jp_ws(P);
            if (P->i < P->n && P->s[P->i] == ',') {
                P->i++;
                continue;
            }
            if (P->i < P->n && P->s[P->i] == ']') {
                P->i++;
                break;
            }
            P->err = 1;
            return NULL;
        }
        v->items = items;
        return v;
    }
    case 't':
        if (P->i + 4 <= P->n && memcmp(P->s + P->i, "true", 4) == 0) {
            JVal *v = jv_new(P, J_BOOL);
            v->b = 1;
            P->i += 4;
            return v;
        }
        P->err = 1;
        return NULL;
    case 'f':
        if (P->i + 5 <= P->n && memcmp(P->s + P->i, "false", 5) == 0) {
            JVal *v = jv_new(P, J_BOOL);
            v->b = 0;
            P->i += 5;
            return v;
        }
        P->err = 1;
        return NULL;
    case 'n':
        if (P->i + 4 <= P->n && memcmp(P->s + P->i, "null", 4) == 0) {
            JVal *v = jv_new(P, J_NULL);
            P->i += 4;
            return v;
        }
        P->err = 1;
        return NULL;
    default:
        if (c == '-' || jp_digit(c))
            return jp_number(P);
        P->err = 1;
        return NULL;
    }
}

/* convert a JSON value to an expression value; NULL if not representable */
static Value *json_to_value(JVal *j, Ctx *C)
{
    switch (j->t) {
    case J_NULL:
        return v_null(C);
    case J_BOOL:
        return v_bool(C, j->b);
    case J_INT:
        if (j->bad)
            return NULL;
        return v_int(C, j->i);
    case J_STR:
        return v_str(C, j->s);
    case J_ARR: {
        Value **items;
        size_t i;
        if (j->nitems == 0)
            return v_arr(C, NULL, 0);
        items = arena_alloc(C->a, j->nitems * sizeof(Value *));
        for (i = 0; i < j->nitems; i++) {
            items[i] = json_to_value(j->items[i], C);
            if (!items[i])
                return NULL;
        }
        return v_arr(C, items, j->nitems);
    }
    case J_OBJ:
        return NULL;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* tokenizer                                                           */
/* ------------------------------------------------------------------ */

typedef enum {
    TK_EOF,
    TK_ERROR,
    TK_INT,
    TK_STR,
    TK_IDENT,
    TK_TRUE,
    TK_FALSE,
    TK_NULL,
    TK_LP,
    TK_RP,
    TK_LB,
    TK_RB,
    TK_COMMA,
    TK_QUESTION,
    TK_COLON,
    TK_PLUS,
    TK_MINUS,
    TK_STAR,
    TK_SLASH,
    TK_PERCENT,
    TK_BANG,
    TK_ANDAND,
    TK_OROR,
    TK_EQ,
    TK_NE,
    TK_LT,
    TK_LE,
    TK_GT,
    TK_GE
} TokKind;

typedef struct {
    TokKind k;
    size_t  pos;    /* byte offset in source */
    Str     text;   /* INT digits, STR bytes, IDENT name */
} Token;

static int is_alpha_us(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int is_alnum_us(unsigned char c)
{
    return is_alpha_us(c) || (c >= '0' && c <= '9');
}

/* ------------------------------------------------------------------ */
/* AST                                                                 */
/* ------------------------------------------------------------------ */

typedef enum {
    N_NULL,
    N_BOOL,
    N_INTLIT,
    N_STR,
    N_VAR,
    N_UNARY,
    N_BIN,
    N_TERN,
    N_ARR,
    N_CALL
} NKind;

typedef struct Node Node;
struct Node {
    NKind k;
    size_t pos;     /* byte offset of the central token */
    union {
        int  b;
        Str  s;     /* literal digits / string / variable / function name */
        struct {
            TokKind op;
            Node   *a;
        } unary;
        struct {
            TokKind op;
            Node   *l, *r;
        } bin;
        struct {
            Node *c, *a, *b;
        } tern;
        struct {
            Node **v;
            size_t n;
        } arr;
        struct {
            Str    name;
            Node **v;
            size_t n;
        } call;
    } u;
};

typedef struct {
    const char *s;
    size_t      n;
    size_t      i;
    Arena      *a;
    Token       tok;
    int         err;
    size_t      epos;
    int         depth;
} Parser;

static Node *node_new(Parser *P, NKind k, size_t pos)
{
    Node *n = arena_alloc(P->a, sizeof(Node));
    memset(n, 0, sizeof(*n));
    n->k = k;
    n->pos = pos;
    return n;
}

static void lex_next(Parser *P)
{
    size_t i = P->i;
    Token t;
    unsigned char c;

    t.k = TK_ERROR;
    t.pos = i;
    t.text.p = NULL;
    t.text.n = 0;

    while (i < P->n) {
        char w = P->s[i];
        if (w == ' ' || w == '\t' || w == '\n' || w == '\r')
            i++;
        else
            break;
    }
    t.pos = i;

    if (i >= P->n) {
        t.k = TK_EOF;
        P->i = i;
        P->tok = t;
        return;
    }

    c = (unsigned char)P->s[i];

    if (c >= '0' && c <= '9') {
        size_t j = i;
        while (j < P->n && P->s[j] >= '0' && P->s[j] <= '9')
            j++;
        t.k = TK_INT;
        t.text.p = P->s + i;
        t.text.n = j - i;
        P->i = j;
        P->tok = t;
        return;
    }

    if (c == '"') {
        Str s;
        size_t end;
        int r = decode_string(P->a, P->s, P->n, i + 1, 0, &end, &s);
        if (r != 0) {
            P->err = 1;
            P->epos = i;
            P->i = i + 1;
            P->tok = t;
            return;
        }
        t.k = TK_STR;
        t.text = s;
        P->i = end;
        P->tok = t;
        return;
    }

    if (is_alpha_us(c)) {
        size_t j = i;
        while (j < P->n && is_alnum_us((unsigned char)P->s[j]))
            j++;
        t.text.p = P->s + i;
        t.text.n = j - i;
        if (t.text.n == 4 && memcmp(t.text.p, "true", 4) == 0)
            t.k = TK_TRUE;
        else if (t.text.n == 5 && memcmp(t.text.p, "false", 5) == 0)
            t.k = TK_FALSE;
        else if (t.text.n == 4 && memcmp(t.text.p, "null", 4) == 0)
            t.k = TK_NULL;
        else
            t.k = TK_IDENT;
        P->i = j;
        P->tok = t;
        return;
    }

#define TWO(a, b) (i + 1 < P->n && P->s[i] == (a) && P->s[i + 1] == (b))
    if (TWO('|', '|')) { t.k = TK_OROR; i += 2; }
    else if (TWO('&', '&')) { t.k = TK_ANDAND; i += 2; }
    else if (TWO('=', '=')) { t.k = TK_EQ; i += 2; }
    else if (TWO('!', '=')) { t.k = TK_NE; i += 2; }
    else if (TWO('<', '=')) { t.k = TK_LE; i += 2; }
    else if (TWO('>', '=')) { t.k = TK_GE; i += 2; }
#undef TWO
    else {
        switch (c) {
        case '(': t.k = TK_LP; i++; break;
        case ')': t.k = TK_RP; i++; break;
        case '[': t.k = TK_LB; i++; break;
        case ']': t.k = TK_RB; i++; break;
        case ',': t.k = TK_COMMA; i++; break;
        case '?': t.k = TK_QUESTION; i++; break;
        case ':': t.k = TK_COLON; i++; break;
        case '+': t.k = TK_PLUS; i++; break;
        case '-': t.k = TK_MINUS; i++; break;
        case '*': t.k = TK_STAR; i++; break;
        case '/': t.k = TK_SLASH; i++; break;
        case '%': t.k = TK_PERCENT; i++; break;
        case '!': t.k = TK_BANG; i++; break;
        case '<': t.k = TK_LT; i++; break;
        case '>': t.k = TK_GT; i++; break;
        default:
            P->err = 1;
            P->epos = i;
            P->i = i + 1;
            P->tok = t;
            return;
        }
    }
    P->i = i;
    P->tok = t;
}

static void p_advance(Parser *P)
{
    if (P->err)
        return;
    lex_next(P);
}

/* ------------------------------------------------------------------ */
/* parser                                                              */
/* ------------------------------------------------------------------ */

static Node *p_expr(Parser *P);

static int p_depth_ok(Parser *P, size_t pos)
{
    if (P->depth > 4096) {
        P->err = 1;
        P->epos = pos;
        return 0;
    }
    return 1;
}

static Node *p_primary(Parser *P)
{
    Node *n;
    Token t;

    if (P->err)
        return NULL;
    if (!p_depth_ok(P, P->tok.pos))
        return NULL;

    t = P->tok;
    switch (t.k) {
    case TK_INT: {
        n = node_new(P, N_INTLIT, t.pos);
        n->u.s = t.text;
        p_advance(P);
        return n;
    }
    case TK_STR: {
        n = node_new(P, N_STR, t.pos);
        n->u.s = t.text;
        p_advance(P);
        return n;
    }
    case TK_TRUE:
    case TK_FALSE: {
        n = node_new(P, N_BOOL, t.pos);
        n->u.b = (t.k == TK_TRUE);
        p_advance(P);
        return n;
    }
    case TK_NULL: {
        n = node_new(P, N_NULL, t.pos);
        p_advance(P);
        return n;
    }
    case TK_IDENT: {
        Str name = t.text;
        size_t npos = t.pos;
        p_advance(P);
        if (!P->err && P->tok.k == TK_LP) {
            size_t cap = 0, cnt = 0;
            Node **items = NULL;
            p_advance(P);
            if (!P->err && P->tok.k == TK_RP) {
                p_advance(P);
            } else {
                for (;;) {
                    Node *item;
                    P->depth++;
                    item = p_expr(P);
                    P->depth--;
                    if (!item)
                        return NULL;
                    if (cnt == cap) {
                        size_t ncap = cap ? cap * 2 : 4;
                        Node **ni = arena_alloc(P->a, ncap * sizeof(Node *));
                        if (cap)
                            memcpy(ni, items, cnt * sizeof(Node *));
                        items = ni;
                        cap = ncap;
                    }
                    items[cnt++] = item;
                    if (!P->err && P->tok.k == TK_COMMA) {
                        p_advance(P);
                        continue;
                    }
                    break;
                }
                if (P->err)
                    return NULL;
                if (P->tok.k != TK_RP) {
                    P->err = 1;
                    P->epos = P->tok.pos;
                    return NULL;
                }
                p_advance(P);
            }
            n = node_new(P, N_CALL, npos);
            n->u.call.name = name;
            n->u.call.v = items;
            n->u.call.n = cnt;
            return n;
        }
        if (P->err)
            return NULL;
        n = node_new(P, N_VAR, npos);
        n->u.s = name;
        return n;
    }
    case TK_LP: {
        p_advance(P);
        P->depth++;
        n = p_expr(P);
        P->depth--;
        if (!n)
            return NULL;
        if (P->tok.k != TK_RP) {
            P->err = 1;
            P->epos = P->tok.pos;
            return NULL;
        }
        p_advance(P);
        return n;
    }
    case TK_LB: {
        size_t cap = 0, cnt = 0;
        Node **items = NULL;
        p_advance(P);
        if (!P->err && P->tok.k == TK_RB) {
            p_advance(P);
        } else {
            for (;;) {
                Node *item;
                P->depth++;
                item = p_expr(P);
                P->depth--;
                if (!item)
                    return NULL;
                if (cnt == cap) {
                    size_t ncap = cap ? cap * 2 : 4;
                    Node **ni = arena_alloc(P->a, ncap * sizeof(Node *));
                    if (cap)
                        memcpy(ni, items, cnt * sizeof(Node *));
                    items = ni;
                    cap = ncap;
                }
                items[cnt++] = item;
                if (!P->err && P->tok.k == TK_COMMA) {
                    p_advance(P);
                    continue;
                }
                break;
            }
            if (P->err)
                return NULL;
            if (P->tok.k != TK_RB) {
                P->err = 1;
                P->epos = P->tok.pos;
                return NULL;
            }
            p_advance(P);
        }
        n = node_new(P, N_ARR, t.pos);
        n->u.arr.v = items;
        n->u.arr.n = cnt;
        return n;
    }
    default:
        P->err = 1;
        P->epos = t.pos;
        return NULL;
    }
}

static Node *p_unary(Parser *P)
{
    Node *n;

    if (P->err)
        return NULL;
    if (P->tok.k == TK_MINUS || P->tok.k == TK_BANG) {
        TokKind op = P->tok.k;
        size_t pos = P->tok.pos;
        p_advance(P);
        P->depth++;
        n = p_unary(P);
        P->depth--;
        if (!n)
            return NULL;
        {
            Node *r = node_new(P, N_UNARY, pos);
            r->u.unary.op = op;
            r->u.unary.a = n;
            return r;
        }
    }
    return p_primary(P);
}

static Node *p_mul(Parser *P)
{
    Node *l;

    if (P->err)
        return NULL;
    l = p_unary(P);
    if (!l)
        return NULL;
    while (!P->err &&
           (P->tok.k == TK_STAR || P->tok.k == TK_SLASH ||
            P->tok.k == TK_PERCENT)) {
        TokKind op = P->tok.k;
        size_t pos = P->tok.pos;
        Node *r, *n;
        p_advance(P);
        r = p_unary(P);
        if (!r)
            return NULL;
        n = node_new(P, N_BIN, pos);
        n->u.bin.op = op;
        n->u.bin.l = l;
        n->u.bin.r = r;
        l = n;
    }
    return P->err ? NULL : l;
}

static Node *p_add(Parser *P)
{
    Node *l;

    if (P->err)
        return NULL;
    l = p_mul(P);
    if (!l)
        return NULL;
    while (!P->err && (P->tok.k == TK_PLUS || P->tok.k == TK_MINUS)) {
        TokKind op = P->tok.k;
        size_t pos = P->tok.pos;
        Node *r, *n;
        p_advance(P);
        r = p_mul(P);
        if (!r)
            return NULL;
        n = node_new(P, N_BIN, pos);
        n->u.bin.op = op;
        n->u.bin.l = l;
        n->u.bin.r = r;
        l = n;
    }
    return P->err ? NULL : l;
}

static Node *p_cmp(Parser *P)
{
    Node *l;

    if (P->err)
        return NULL;
    l = p_add(P);
    if (!l)
        return NULL;
    while (!P->err &&
           (P->tok.k == TK_LT || P->tok.k == TK_LE ||
            P->tok.k == TK_GT || P->tok.k == TK_GE)) {
        TokKind op = P->tok.k;
        size_t pos = P->tok.pos;
        Node *r, *n;
        p_advance(P);
        r = p_add(P);
        if (!r)
            return NULL;
        n = node_new(P, N_BIN, pos);
        n->u.bin.op = op;
        n->u.bin.l = l;
        n->u.bin.r = r;
        l = n;
    }
    return P->err ? NULL : l;
}

static Node *p_eq(Parser *P)
{
    Node *l;

    if (P->err)
        return NULL;
    l = p_cmp(P);
    if (!l)
        return NULL;
    while (!P->err && (P->tok.k == TK_EQ || P->tok.k == TK_NE)) {
        TokKind op = P->tok.k;
        size_t pos = P->tok.pos;
        Node *r, *n;
        p_advance(P);
        r = p_cmp(P);
        if (!r)
            return NULL;
        n = node_new(P, N_BIN, pos);
        n->u.bin.op = op;
        n->u.bin.l = l;
        n->u.bin.r = r;
        l = n;
    }
    return P->err ? NULL : l;
}

static Node *p_and(Parser *P)
{
    Node *l;

    if (P->err)
        return NULL;
    l = p_eq(P);
    if (!l)
        return NULL;
    while (!P->err && P->tok.k == TK_ANDAND) {
        size_t pos = P->tok.pos;
        Node *r, *n;
        p_advance(P);
        r = p_eq(P);
        if (!r)
            return NULL;
        n = node_new(P, N_BIN, pos);
        n->u.bin.op = TK_ANDAND;
        n->u.bin.l = l;
        n->u.bin.r = r;
        l = n;
    }
    return P->err ? NULL : l;
}

static Node *p_or(Parser *P)
{
    Node *l;

    if (P->err)
        return NULL;
    l = p_and(P);
    if (!l)
        return NULL;
    while (!P->err && P->tok.k == TK_OROR) {
        size_t pos = P->tok.pos;
        Node *r, *n;
        p_advance(P);
        r = p_and(P);
        if (!r)
            return NULL;
        n = node_new(P, N_BIN, pos);
        n->u.bin.op = TK_OROR;
        n->u.bin.l = l;
        n->u.bin.r = r;
        l = n;
    }
    return P->err ? NULL : l;
}

static Node *p_ternary(Parser *P)
{
    Node *c;

    if (P->err)
        return NULL;
    c = p_or(P);
    if (!c)
        return NULL;
    if (!P->err && P->tok.k == TK_QUESTION) {
        size_t qpos = P->tok.pos;
        Node *a, *b, *n;
        p_advance(P);
        P->depth++;
        a = p_expr(P);
        P->depth--;
        if (!a)
            return NULL;
        if (P->tok.k != TK_COLON) {
            P->err = 1;
            P->epos = P->tok.pos;
            return NULL;
        }
        p_advance(P);
        P->depth++;
        b = p_expr(P);
        P->depth--;
        if (!b)
            return NULL;
        n = node_new(P, N_TERN, qpos);
        n->u.tern.c = c;
        n->u.tern.a = a;
        n->u.tern.b = b;
        return n;
    }
    return P->err ? NULL : c;
}

static Node *p_expr(Parser *P)
{
    return p_ternary(P);
}

/* ------------------------------------------------------------------ */
/* evaluation                                                          */
/* ------------------------------------------------------------------ */

static void render_value(Buf *b, const Value *v);

/* parse an unsigned decimal digit string into int64; 0 on overflow */
static int parse_i64_digits(const char *p, size_t n, int64_t *out)
{
    uint64_t acc = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned d = (unsigned)(p[i] - '0');
        if (acc > ((uint64_t)INT64_MAX - d) / 10u)
            return 0;
        acc = acc * 10u + d;
    }
    *out = (int64_t)acc;
    return 1;
}

static Value *eval_node(Node *n, Ctx *C, Err *e);

/* --- functions ---------------------------------------------------- */

#define FN_ARGS Ctx *C, Value **args, size_t nargs, size_t fnpos, Err *e
#define FN_UNUSED do { (void)nargs; (void)fnpos; } while (0)

static Value *fn_len(FN_ARGS)
{
    FN_UNUSED;
    if (args[0]->t == VT_STR)
        return v_int(C, (int64_t)utf8_cp_count(args[0]->u.s.p, args[0]->u.s.n));
    if (args[0]->t == VT_ARR)
        return v_int(C, (int64_t)args[0]->u.arr.n);
    set_err(e, ERR_TYPE, fnpos);
    return NULL;
}

static Value *fn_sum(FN_ARGS)
{
    int64_t acc = 0;
    size_t i;
    FN_UNUSED;
    if (args[0]->t != VT_ARR) {
        set_err(e, ERR_TYPE, fnpos);
        return NULL;
    }
    for (i = 0; i < args[0]->u.arr.n; i++) {
        int64_t z;
        if (args[0]->u.arr.v[i]->t != VT_INT) {
            set_err(e, ERR_TYPE, fnpos);
            return NULL;
        }
        if (__builtin_add_overflow(acc, args[0]->u.arr.v[i]->u.i, &z)) {
            set_err(e, ERR_OVERFLOW, fnpos);
            return NULL;
        }
        acc = z;
    }
    return v_int(C, acc);
}

static Value *fn_min(FN_ARGS)
{
    int64_t m;
    size_t i;
    FN_UNUSED;
    if (args[0]->t != VT_ARR) {
        set_err(e, ERR_TYPE, fnpos);
        return NULL;
    }
    if (args[0]->u.arr.n == 0) {
        set_err(e, ERR_EMPTY_ARRAY, fnpos);
        return NULL;
    }
    m = 0;
    for (i = 0; i < args[0]->u.arr.n; i++) {
        int64_t x;
        if (args[0]->u.arr.v[i]->t != VT_INT) {
            set_err(e, ERR_TYPE, fnpos);
            return NULL;
        }
        x = args[0]->u.arr.v[i]->u.i;
        if (i == 0 || x < m)
            m = x;
    }
    return v_int(C, m);
}

static Value *fn_max(FN_ARGS)
{
    int64_t m;
    size_t i;
    FN_UNUSED;
    if (args[0]->t != VT_ARR) {
        set_err(e, ERR_TYPE, fnpos);
        return NULL;
    }
    if (args[0]->u.arr.n == 0) {
        set_err(e, ERR_EMPTY_ARRAY, fnpos);
        return NULL;
    }
    m = 0;
    for (i = 0; i < args[0]->u.arr.n; i++) {
        int64_t x;
        if (args[0]->u.arr.v[i]->t != VT_INT) {
            set_err(e, ERR_TYPE, fnpos);
            return NULL;
        }
        x = args[0]->u.arr.v[i]->u.i;
        if (i == 0 || x > m)
            m = x;
    }
    return v_int(C, m);
}

static Value *fn_abs(FN_ARGS)
{
    int64_t x;
    FN_UNUSED;
    if (args[0]->t != VT_INT) {
        set_err(e, ERR_TYPE, fnpos);
        return NULL;
    }
    x = args[0]->u.i;
    if (x == INT64_MIN) {
        set_err(e, ERR_OVERFLOW, fnpos);
        return NULL;
    }
    return v_int(C, x < 0 ? -x : x);
}

static Value *fn_neg(FN_ARGS)
{
    int64_t x;
    FN_UNUSED;
    if (args[0]->t != VT_INT) {
        set_err(e, ERR_TYPE, fnpos);
        return NULL;
    }
    x = args[0]->u.i;
    if (x == INT64_MIN) {
        set_err(e, ERR_OVERFLOW, fnpos);
        return NULL;
    }
    return v_int(C, -x);
}

static Value *fn_upper(FN_ARGS)
{
    size_t i;
    char *p;
    FN_UNUSED;
    if (args[0]->t != VT_STR) {
        set_err(e, ERR_TYPE, fnpos);
        return NULL;
    }
    p = arena_alloc(C->a, args[0]->u.s.n ? args[0]->u.s.n : 1);
    for (i = 0; i < args[0]->u.s.n; i++) {
        unsigned char c = (unsigned char)args[0]->u.s.p[i];
        p[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : (char)c;
    }
    {
        Str s;
        s.p = p;
        s.n = args[0]->u.s.n;
        return v_str(C, s);
    }
}

static Value *fn_lower(FN_ARGS)
{
    size_t i;
    char *p;
    FN_UNUSED;
    if (args[0]->t != VT_STR) {
        set_err(e, ERR_TYPE, fnpos);
        return NULL;
    }
    p = arena_alloc(C->a, args[0]->u.s.n ? args[0]->u.s.n : 1);
    for (i = 0; i < args[0]->u.s.n; i++) {
        unsigned char c = (unsigned char)args[0]->u.s.p[i];
        p[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : (char)c;
    }
    {
        Str s;
        s.p = p;
        s.n = args[0]->u.s.n;
        return v_str(C, s);
    }
}

static int is_trim_ws(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\f' || c == '\v';
}

static Value *fn_trim(FN_ARGS)
{
    Str s;
    FN_UNUSED;
    if (args[0]->t != VT_STR) {
        set_err(e, ERR_TYPE, fnpos);
        return NULL;
    }
    s = args[0]->u.s;
    while (s.n > 0 && is_trim_ws((unsigned char)s.p[0])) {
        s.p++;
        s.n--;
    }
    while (s.n > 0 && is_trim_ws((unsigned char)s.p[s.n - 1]))
        s.n--;
    return v_str(C, s);
}

/* find sub in s; returns index or -1 */
static long mem_find(Str s, Str sub)
{
    size_t i;
    if (sub.n == 0)
        return 0;
    if (sub.n > s.n)
        return -1;
    for (i = 0; i + sub.n <= s.n; i++) {
        if (memcmp(s.p + i, sub.p, sub.n) == 0)
            return (long)i;
    }
    return -1;
}

static Value *fn_contains(FN_ARGS)
{
    FN_UNUSED;
    if (args[0]->t != VT_STR || args[1]->t != VT_STR) {
        set_err(e, ERR_TYPE, fnpos);
        return NULL;
    }
    return v_bool(C, mem_find(args[0]->u.s, args[1]->u.s) >= 0);
}

static Value *fn_starts_with(FN_ARGS)
{
    FN_UNUSED;
    if (args[0]->t != VT_STR || args[1]->t != VT_STR) {
        set_err(e, ERR_TYPE, fnpos);
        return NULL;
    }
    if (args[1]->u.s.n > args[0]->u.s.n)
        return v_bool(C, 0);
    return v_bool(C, memcmp(args[0]->u.s.p, args[1]->u.s.p,
                            args[1]->u.s.n) == 0);
}

static Value *fn_ends_with(FN_ARGS)
{
    Str a, b;
    FN_UNUSED;
    if (args[0]->t != VT_STR || args[1]->t != VT_STR) {
        set_err(e, ERR_TYPE, fnpos);
        return NULL;
    }
    a = args[0]->u.s;
    b = args[1]->u.s;
    if (b.n > a.n)
        return v_bool(C, 0);
    return v_bool(C, memcmp(a.p + (a.n - b.n), b.p, b.n) == 0);
}

static Value *fn_substr(FN_ARGS)
{
    Str s;
    int64_t start, len;
    size_t total, ustart, ulen;

    FN_UNUSED;
    if (args[0]->t != VT_STR || args[1]->t != VT_INT || args[2]->t != VT_INT) {
        set_err(e, ERR_TYPE, fnpos);
        return NULL;
    }
    start = args[1]->u.i;
    len = args[2]->u.i;
    if (len < 0) {
        set_err(e, ERR_TYPE, fnpos);
        return NULL;
    }
    s = args[0]->u.s;
    total = utf8_cp_count(s.p, s.n);

    if (start < 0) {
        if (start < -(int64_t)total)
            ustart = 0;
        else
            ustart = (size_t)((int64_t)total + start);
    } else {
        if ((uint64_t)start > (uint64_t)total)
            ustart = total;
        else
            ustart = (size_t)start;
    }
    if ((uint64_t)len > (uint64_t)(total - ustart))
        ulen = total - ustart;
    else
        ulen = (size_t)len;

    return v_str(C, str_slice_cp(s, ustart, ulen));
}

static Value *fn_index_of(FN_ARGS)
{
    Str s, sub;
    long idx;

    FN_UNUSED;
    if (args[0]->t != VT_STR || args[1]->t != VT_STR) {
        set_err(e, ERR_TYPE, fnpos);
        return NULL;
    }
    s = args[0]->u.s;
    sub = args[1]->u.s;
    if (sub.n == 0)
        return v_int(C, 0);
    idx = mem_find(s, sub);
    if (idx < 0)
        return v_int(C, -1);
    return v_int(C, (int64_t)utf8_cp_count(s.p, (size_t)idx));
}

static Value *fn_join(FN_ARGS)
{
    Str sep;
    Buf b;
    size_t i, total = 0;
    char *p;
    Str out;

    FN_UNUSED;
    if (args[0]->t != VT_ARR || args[1]->t != VT_STR) {
        set_err(e, ERR_TYPE, fnpos);
        return NULL;
    }
    sep = args[1]->u.s;
    for (i = 0; i < args[0]->u.arr.n; i++) {
        if (args[0]->u.arr.v[i]->t != VT_STR) {
            set_err(e, ERR_TYPE, fnpos);
            return NULL;
        }
        total += args[0]->u.arr.v[i]->u.s.n;
        if (i + 1 < args[0]->u.arr.n)
            total += sep.n;
    }
    if (total == 0) {
        out.p = arena_alloc(C->a, 1);
        out.n = 0;
        return v_str(C, out);
    }
    p = arena_alloc(C->a, total);
    buf_init(&b);
    b.p = p;
    b.cap = total;
    for (i = 0; i < args[0]->u.arr.n; i++) {
        if (i > 0)
            buf_add(&b, sep.p, sep.n);
        buf_add(&b, args[0]->u.arr.v[i]->u.s.p, args[0]->u.arr.v[i]->u.s.n);
    }
    out.p = p;
    out.n = b.n;
    return v_str(C, out);
}

static Value *fn_split(FN_ARGS)
{
    Str s, sep;
    Value **items;
    size_t cnt = 0;

    FN_UNUSED;
    if (args[0]->t != VT_STR || args[1]->t != VT_STR) {
        set_err(e, ERR_TYPE, fnpos);
        return NULL;
    }
    s = args[0]->u.s;
    sep = args[1]->u.s;

    if (sep.n == 0) {
        size_t total = utf8_cp_count(s.p, s.n);
        size_t i, off = 0;
        items = arena_alloc(C->a, (total ? total : 1) * sizeof(Value *));
        for (i = 0; i < total; i++) {
            size_t l = u8_len((unsigned char)s.p[off]);
            Str piece;
            if (off + l > s.n)
                l = s.n - off;
            piece.p = s.p + off;
            piece.n = l;
            items[cnt++] = v_str(C, piece);
            off += l;
        }
        return v_arr(C, items, cnt);
    }

    {
        size_t cap = 8;
        size_t start = 0, i = 0;
        items = arena_alloc(C->a, cap * sizeof(Value *));
        while (i + sep.n <= s.n) {
            if (memcmp(s.p + i, sep.p, sep.n) == 0) {
                Str piece;
                piece.p = s.p + start;
                piece.n = i - start;
                if (cnt == cap) {
                    size_t ncap = cap * 2;
                    Value **ni = arena_alloc(C->a, ncap * sizeof(Value *));
                    memcpy(ni, items, cnt * sizeof(Value *));
                    items = ni;
                    cap = ncap;
                }
                items[cnt++] = v_str(C, piece);
                i += sep.n;
                start = i;
            } else {
                i++;
            }
        }
        {
            Str piece;
            piece.p = s.p + start;
            piece.n = s.n - start;
            if (cnt == cap) {
                size_t ncap = cap * 2;
                Value **ni = arena_alloc(C->a, ncap * sizeof(Value *));
                memcpy(ni, items, cnt * sizeof(Value *));
                items = ni;
                cap = ncap;
            }
            items[cnt++] = v_str(C, piece);
        }
    }
    return v_arr(C, items, cnt);
}

static Value *fn_type(FN_ARGS)
{
    const char *name;
    Str s;
    FN_UNUSED;
    (void)e;
    switch (args[0]->t) {
    case VT_NULL: name = "null"; break;
    case VT_BOOL: name = "boolean"; break;
    case VT_INT: name = "integer"; break;
    case VT_STR: name = "string"; break;
    default: name = "array"; break;
    }
    s.p = name;
    s.n = strlen(name);
    return v_str(C, s);
}

static Value *fn_to_string(FN_ARGS)
{
    Value *v = args[0];
    FN_UNUSED;
    (void)e;
    if (v->t == VT_STR)
        return v_str(C, v->u.s);
    {
        Buf b;
        Str s;
        char *p;
        buf_init(&b);
        render_value(&b, v);
        p = arena_alloc(C->a, b.n ? b.n : 1);
        if (b.n)
            memcpy(p, b.p, b.n);
        s.p = p;
        s.n = b.n;
        buf_free(&b);
        return v_str(C, s);
    }
}

typedef Value *(*FnPtr)(FN_ARGS);

typedef struct {
    const char *name;
    size_t      nargs;
    FnPtr       fn;
} FuncDef;

static const FuncDef FUNCS[] = {
    { "len",         1, fn_len },
    { "sum",         1, fn_sum },
    { "min",         1, fn_min },
    { "max",         1, fn_max },
    { "abs",         1, fn_abs },
    { "neg",         1, fn_neg },
    { "upper",       1, fn_upper },
    { "lower",       1, fn_lower },
    { "trim",        1, fn_trim },
    { "contains",    2, fn_contains },
    { "starts_with", 2, fn_starts_with },
    { "ends_with",   2, fn_ends_with },
    { "substr",      3, fn_substr },
    { "index_of",    2, fn_index_of },
    { "join",        2, fn_join },
    { "split",       2, fn_split },
    { "type",        1, fn_type },
    { "to_string",   1, fn_to_string }
};

static Value *eval_call(Node *n, Ctx *C, Err *e)
{
    size_t i;
    Value **args;
    const FuncDef *fd = NULL;

    args = n->u.call.n ? arena_alloc(C->a, n->u.call.n * sizeof(Value *)) : NULL;
    for (i = 0; i < n->u.call.n; i++) {
        args[i] = eval_node(n->u.call.v[i], C, e);
        if (!args[i])
            return NULL;
    }

    for (i = 0; i < sizeof(FUNCS) / sizeof(FUNCS[0]); i++) {
        if (strlen(FUNCS[i].name) == n->u.call.name.n &&
            memcmp(FUNCS[i].name, n->u.call.name.p, n->u.call.name.n) == 0) {
            fd = &FUNCS[i];
            break;
        }
    }
    if (!fd) {
        set_err(e, ERR_UNKNOWN_FUNCTION, n->pos);
        return NULL;
    }
    if (n->u.call.n != fd->nargs) {
        set_err(e, ERR_ARITY, n->pos);
        return NULL;
    }
    return fd->fn(C, args, n->u.call.n, n->pos, e);
}

static Value *eval_binary(Node *n, Ctx *C, Err *e)
{
    TokKind op = n->u.bin.op;
    Value *l, *r;

    l = eval_node(n->u.bin.l, C, e);
    if (!l)
        return NULL;

    if (op == TK_ANDAND || op == TK_OROR) {
        if (l->t != VT_BOOL) {
            set_err(e, ERR_TYPE, n->pos);
            return NULL;
        }
        if (op == TK_ANDAND && !l->u.b)
            return v_bool(C, 0);
        if (op == TK_OROR && l->u.b)
            return v_bool(C, 1);
        r = eval_node(n->u.bin.r, C, e);
        if (!r)
            return NULL;
        if (r->t != VT_BOOL) {
            set_err(e, ERR_TYPE, n->pos);
            return NULL;
        }
        return v_bool(C, r->u.b);
    }

    r = eval_node(n->u.bin.r, C, e);
    if (!r)
        return NULL;

    switch (op) {
    case TK_PLUS:
    case TK_MINUS:
    case TK_STAR:
    case TK_SLASH:
    case TK_PERCENT: {
        int64_t x, y, z;
        if (l->t != VT_INT || r->t != VT_INT) {
            set_err(e, ERR_TYPE, n->pos);
            return NULL;
        }
        x = l->u.i;
        y = r->u.i;
        switch (op) {
        case TK_PLUS:
            if (__builtin_add_overflow(x, y, &z)) {
                set_err(e, ERR_OVERFLOW, n->pos);
                return NULL;
            }
            break;
        case TK_MINUS:
            if (__builtin_sub_overflow(x, y, &z)) {
                set_err(e, ERR_OVERFLOW, n->pos);
                return NULL;
            }
            break;
        case TK_STAR:
            if (__builtin_mul_overflow(x, y, &z)) {
                set_err(e, ERR_OVERFLOW, n->pos);
                return NULL;
            }
            break;
        case TK_SLASH:
            if (y == 0) {
                set_err(e, ERR_DIVISION_BY_ZERO, n->pos);
                return NULL;
            }
            if (x == INT64_MIN && y == -1) {
                set_err(e, ERR_OVERFLOW, n->pos);
                return NULL;
            }
            z = x / y;
            break;
        default:
            if (y == 0) {
                set_err(e, ERR_DIVISION_BY_ZERO, n->pos);
                return NULL;
            }
            if (x == INT64_MIN && y == -1) {
                set_err(e, ERR_OVERFLOW, n->pos);
                return NULL;
            }
            z = x % y;
            break;
        }
        return v_int(C, z);
    }
    case TK_LT:
    case TK_LE:
    case TK_GT:
    case TK_GE: {
        int c;
        if (l->t == VT_INT && r->t == VT_INT) {
            c = l->u.i < r->u.i ? -1 : (l->u.i > r->u.i ? 1 : 0);
        } else if (l->t == VT_STR && r->t == VT_STR) {
            c = str_compare(l->u.s, r->u.s);
        } else {
            set_err(e, ERR_TYPE, n->pos);
            return NULL;
        }
        switch (op) {
        case TK_LT: return v_bool(C, c < 0);
        case TK_LE: return v_bool(C, c <= 0);
        case TK_GT: return v_bool(C, c > 0);
        default:    return v_bool(C, c >= 0);
        }
    }
    case TK_EQ:
        return v_bool(C, value_equal(l, r));
    case TK_NE:
        return v_bool(C, !value_equal(l, r));
    default:
        set_err(e, ERR_INTERNAL, NOPOS);
        return NULL;
    }
}

static Value *eval_node(Node *n, Ctx *C, Err *e)
{
    switch (n->k) {
    case N_NULL:
        return v_null(C);
    case N_BOOL:
        return v_bool(C, n->u.b);
    case N_INTLIT: {
        int64_t x;
        if (!parse_i64_digits(n->u.s.p, n->u.s.n, &x)) {
            set_err(e, ERR_OVERFLOW, n->pos);
            return NULL;
        }
        return v_int(C, x);
    }
    case N_STR:
        return v_str(C, n->u.s);
    case N_VAR: {
        Value *v = lookup_var(C, n->u.s);
        if (!v) {
            set_err(e, ERR_UNKNOWN_VARIABLE, n->pos);
            return NULL;
        }
        return v;
    }
    case N_ARR: {
        Value **items;
        size_t i;
        if (n->u.arr.n == 0)
            return v_arr(C, NULL, 0);
        items = arena_alloc(C->a, n->u.arr.n * sizeof(Value *));
        for (i = 0; i < n->u.arr.n; i++) {
            items[i] = eval_node(n->u.arr.v[i], C, e);
            if (!items[i])
                return NULL;
        }
        return v_arr(C, items, n->u.arr.n);
    }
    case N_UNARY: {
        Value *a = eval_node(n->u.unary.a, C, e);
        if (!a)
            return NULL;
        if (n->u.unary.op == TK_BANG) {
            if (a->t != VT_BOOL) {
                set_err(e, ERR_TYPE, n->pos);
                return NULL;
            }
            return v_bool(C, !a->u.b);
        }
        if (a->t != VT_INT) {
            set_err(e, ERR_TYPE, n->pos);
            return NULL;
        }
        if (a->u.i == INT64_MIN) {
            set_err(e, ERR_OVERFLOW, n->pos);
            return NULL;
        }
        return v_int(C, -a->u.i);
    }
    case N_BIN:
        return eval_binary(n, C, e);
    case N_TERN: {
        Value *c = eval_node(n->u.tern.c, C, e);
        if (!c)
            return NULL;
        if (c->t != VT_BOOL) {
            set_err(e, ERR_TYPE, n->pos);
            return NULL;
        }
        return eval_node(c->u.b ? n->u.tern.a : n->u.tern.b, C, e);
    }
    case N_CALL:
        return eval_call(n, C, e);
    }
    set_err(e, ERR_INTERNAL, NOPOS);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* rendering                                                           */
/* ------------------------------------------------------------------ */

static void render_json_string(Buf *b, Str s)
{
    size_t i;
    static const char hex[] = "0123456789abcdef";
    buf_addc(b, '"');
    for (i = 0; i < s.n; i++) {
        unsigned char c = (unsigned char)s.p[i];
        switch (c) {
        case '"':  buf_adds(b, "\\\""); break;
        case '\\': buf_adds(b, "\\\\"); break;
        case '\n': buf_adds(b, "\\n"); break;
        case '\t': buf_adds(b, "\\t"); break;
        case '\r': buf_adds(b, "\\r"); break;
        case '\b': buf_adds(b, "\\b"); break;
        case '\f': buf_adds(b, "\\f"); break;
        default:
            if (c < 0x20) {
                char tmp[6];
                tmp[0] = '\\';
                tmp[1] = 'u';
                tmp[2] = '0';
                tmp[3] = '0';
                tmp[4] = hex[(c >> 4) & 0xF];
                tmp[5] = hex[c & 0xF];
                buf_add(b, tmp, 6);
            } else {
                buf_addc(b, (char)c);
            }
            break;
        }
    }
    buf_addc(b, '"');
}

static void render_value(Buf *b, const Value *v)
{
    switch (v->t) {
    case VT_NULL:
        buf_adds(b, "null");
        break;
    case VT_BOOL:
        buf_adds(b, v->u.b ? "true" : "false");
        break;
    case VT_INT: {
        char tmp[32];
        int n = snprintf(tmp, sizeof(tmp), "%" PRId64, v->u.i);
        buf_add(b, tmp, (size_t)n);
        break;
    }
    case VT_STR:
        render_json_string(b, v->u.s);
        break;
    case VT_ARR: {
        size_t i;
        buf_addc(b, '[');
        for (i = 0; i < v->u.arr.n; i++) {
            if (i)
                buf_addc(b, ',');
            render_value(b, v->u.arr.v[i]);
        }
        buf_addc(b, ']');
        break;
    }
    }
}

static void print_error(const Err *e, const char *src, size_t srclen, int code)
{
    Buf b;
    buf_init(&b);
    buf_adds(&b, "{\"status\":\"error\",\"code\":\"");
    buf_adds(&b, err_code_name(code));
    buf_adds(&b, "\",\"position\":");
    if (code == ERR_INVALID_REQUEST || e->pos == NOPOS) {
        buf_adds(&b, "null");
    } else {
        char tmp[32];
        size_t cp = utf8_cp_count(src, e->pos <= srclen ? e->pos : srclen);
        int n = snprintf(tmp, sizeof(tmp), "%zu", cp);
        buf_add(&b, tmp, (size_t)n);
    }
    buf_adds(&b, ",\"message\":\"");
    buf_adds(&b, err_message(code));
    buf_adds(&b, "\"}\n");
    fwrite(b.p, 1, b.n, stdout);
    buf_free(&b);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static int read_stream(FILE *f, Buf *b)
{
    char tmp[65536];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0)
        buf_add(b, tmp, n);
    if (ferror(f))
        return 0;
    return 1;
}

int main(int argc, char **argv)
{
    Buf input;
    Arena arena;
    JP jp;
    JVal *root;
    Ctx C;
    Err err;
    const char *bad_usage = NULL;
    int have_source = 0;
    JVal *source_j = NULL;
    JVal *vars_j = NULL;
    size_t i;
    Node *ast;
    Parser P;

    arena.head = NULL;
    buf_init(&input);

    if (argc != 3 || strcmp(argv[1], "eval") != 0) {
        bad_usage = "usage: expr eval <request.json|->";
    } else if (strcmp(argv[2], "-") == 0) {
        if (!read_stream(stdin, &input))
            bad_usage = "cannot read stdin";
    } else {
        FILE *f = fopen(argv[2], "rb");
        if (!f) {
            bad_usage = "cannot open request file";
        } else {
            int ok = read_stream(f, &input);
            fclose(f);
            if (!ok)
                bad_usage = "cannot read request file";
        }
    }

    if (bad_usage) {
        Err e;
        e.code = ERR_INVALID_REQUEST;
        e.pos = NOPOS;
        fprintf(stderr, "expr: %s\n", bad_usage);
        print_error(&e, "", 0, ERR_INVALID_REQUEST);
        buf_free(&input);
        return 2;
    }

    memset(&jp, 0, sizeof(jp));
    jp.s = input.p;
    jp.n = input.n;
    jp.a = &arena;
    root = jp_value(&jp, 0);
    if (root) {
        jp_ws(&jp);
        if (jp.i != jp.n)
            jp.err = 1;
    }
    if (!root || jp.err || root->t != J_OBJ) {
        Err e;
        e.code = ERR_INVALID_REQUEST;
        e.pos = NOPOS;
        print_error(&e, "", 0, ERR_INVALID_REQUEST);
        buf_free(&input);
        arena_free(&arena);
        return 2;
    }

    for (i = 0; i < root->nkeys; i++) {
        Str k = root->keys[i];
        if (k.n == 6 && memcmp(k.p, "source", 6) == 0) {
            if (root->vals[i]->t != J_STR) {
                bad_usage = "source must be a string";
                break;
            }
            source_j = root->vals[i];
            have_source = 1;
        } else if (k.n == 4 && memcmp(k.p, "vars", 4) == 0) {
            if (root->vals[i]->t != J_OBJ) {
                bad_usage = "vars must be an object";
                break;
            }
            vars_j = root->vals[i];
        } else {
            bad_usage = "unknown request field";
            break;
        }
    }
    if (!have_source && !bad_usage)
        bad_usage = "missing source field";

    if (bad_usage) {
        Err e;
        e.code = ERR_INVALID_REQUEST;
        e.pos = NOPOS;
        fprintf(stderr, "expr: %s\n", bad_usage);
        print_error(&e, "", 0, ERR_INVALID_REQUEST);
        buf_free(&input);
        arena_free(&arena);
        return 2;
    }

    memset(&C, 0, sizeof(C));
    C.a = &arena;
    C.src = source_j->s.p;
    C.srclen = source_j->s.n;

    if (vars_j && vars_j->nkeys) {
        C.vars = arena_alloc(&arena, vars_j->nkeys * sizeof(Var));
        C.nvars = 0;
        for (i = 0; i < vars_j->nkeys; i++) {
            Value *v = json_to_value(vars_j->vals[i], &C);
            if (!v) {
                Err e;
                e.code = ERR_INVALID_REQUEST;
                e.pos = NOPOS;
                fprintf(stderr, "expr: unsupported variable value\n");
                print_error(&e, "", 0, ERR_INVALID_REQUEST);
                buf_free(&input);
                arena_free(&arena);
                return 2;
            }
            C.vars[C.nvars].name = vars_j->keys[i];
            C.vars[C.nvars].val = v;
            C.nvars++;
        }
    }

    memset(&P, 0, sizeof(P));
    P.s = C.src;
    P.n = C.srclen;
    P.a = &arena;
    lex_next(&P);
    ast = p_expr(&P);
    if (ast && !P.err && P.tok.k != TK_EOF) {
        P.err = 1;
        P.epos = P.tok.pos;
    }
    if (!ast || P.err) {
        Err e;
        e.code = ERR_NONE;
        e.pos = P.err ? P.epos : NOPOS;
        print_error(&e, C.src, C.srclen, ERR_PARSE);
        buf_free(&input);
        arena_free(&arena);
        return 1;
    }

    memset(&err, 0, sizeof(err));
    err.code = ERR_NONE;
    err.pos = NOPOS;
    {
        Value *res = eval_node(ast, &C, &err);
        if (!res) {
            int code = err.code ? err.code : ERR_INTERNAL;
            print_error(&err, C.src, C.srclen, code);
            buf_free(&input);
            arena_free(&arena);
            return err_exit_code(code);
        }
        {
            Buf out;
            buf_init(&out);
            buf_adds(&out, "{\"status\":\"ok\",\"value\":");
            render_value(&out, res);
            buf_adds(&out, "}\n");
            fwrite(out.p, 1, out.n, stdout);
            buf_free(&out);
        }
    }

    buf_free(&input);
    arena_free(&arena);
    return 0;
}
