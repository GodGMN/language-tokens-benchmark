/*
 * workflow.c - declarative workflow engine (SPEC.md implementation)
 *
 * Single translation unit, no external dependencies beyond the C standard
 * library.  Includes its own small JSON parser / serializer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <limits.h>

typedef long long i64;

_Static_assert(sizeof(i64) == 8, "i64 must be 64 bits");
_Static_assert(sizeof(unsigned long long) == 8, "ull must be 64 bits");

/* ------------------------------------------------------------------ */
/* memory                                                              */
/* ------------------------------------------------------------------ */

static void internal_oom(void)
{
    fputs("{\"status\":\"failed\",\"results\":{},\"execution_order\":[],"
          "\"skipped\":[],\"error\":{\"code\":\"INTERNAL\",\"step\":null,"
          "\"message\":\"out of memory\"}}\n", stdout);
    fflush(stdout);
    exit(3);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) internal_oom();
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) internal_oom();
    return q;
}

static void *xcalloc(size_t n, size_t sz)
{
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) internal_oom();
    return p;
}

static char *xstrndup(const char *s, size_t n)
{
    char *p = xmalloc(n + 1);
    if (n) memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* ------------------------------------------------------------------ */
/* growable byte buffer                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    char *p;
    size_t len, cap;
} Buf;

static void bneed(Buf *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap) return;
    size_t nc = b->cap ? b->cap : 64;
    while (nc < b->len + extra + 1) {
        if (nc > (size_t)-1 / 2) internal_oom();
        nc *= 2;
    }
    b->p = xrealloc(b->p, nc);
    b->cap = nc;
}

static void bputc(Buf *b, char c)
{
    bneed(b, 1);
    b->p[b->len++] = c;
    b->p[b->len] = 0;
}

static void bwrite(Buf *b, const char *s, size_t n)
{
    if (!n) return;
    bneed(b, n);
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void bputs(Buf *b, const char *s)
{
    bwrite(b, s, strlen(s));
}

static void bput_utf8(Buf *b, unsigned cp)
{
    if (cp < 0x80) {
        bputc(b, (char)cp);
    } else if (cp < 0x800) {
        bputc(b, (char)(0xC0 | (cp >> 6)));
        bputc(b, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        bputc(b, (char)(0xE0 | (cp >> 12)));
        bputc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        bputc(b, (char)(0x80 | (cp & 0x3F)));
    } else {
        bputc(b, (char)(0xF0 | (cp >> 18)));
        bputc(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
        bputc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        bputc(b, (char)(0x80 | (cp & 0x3F)));
    }
}

/* ------------------------------------------------------------------ */
/* JSON value                                                          */
/* ------------------------------------------------------------------ */

enum {
    J_NULL = 0,
    J_BOOL,
    J_NUM,
    J_STR,
    J_ARR,
    J_OBJ
};

typedef struct JVal JVal;

typedef struct {
    char *k;
    size_t klen;
    JVal *v;
} JEnt;

struct JVal {
    int type;
    int b;          /* boolean */
    int is_int;     /* number is an integer literal fitting in i64 */
    i64 i;          /* integer value when is_int */
    double d;       /* floating value (all numbers) */
    char *s;        /* string bytes, or raw number text */
    size_t slen;
    JVal **arr;     /* array elements */
    size_t alen, acap;
    JEnt *ent;      /* object entries, insertion order */
    size_t elen, ecap;
};

static JVal *jnew(int type)
{
    JVal *v = xmalloc(sizeof(*v));
    memset(v, 0, sizeof(*v));
    v->type = type;
    return v;
}

static JVal *jnull(void)
{
    static JVal *n = NULL;
    if (!n) n = jnew(J_NULL);
    return n;
}

static JVal *jbool(int b)
{
    JVal *v = jnew(J_BOOL);
    v->b = b ? 1 : 0;
    return v;
}

static JVal *jnum(i64 i)
{
    JVal *v = jnew(J_NUM);
    v->is_int = 1;
    v->i = i;
    v->d = (double)i;
    return v;
}

static JVal *jstr(const char *s, size_t n)
{
    JVal *v = jnew(J_STR);
    v->s = xstrndup(s, n);
    v->slen = n;
    return v;
}

static JVal *jstrz(const char *s)
{
    return jstr(s, strlen(s));
}

static JVal *jarr(void)
{
    return jnew(J_ARR);
}

static JVal *jobj(void)
{
    return jnew(J_OBJ);
}

static void arr_push(JVal *a, JVal *v)
{
    if (a->alen == a->acap) {
        a->acap = a->acap ? a->acap * 2 : 4;
        a->arr = xrealloc(a->arr, a->acap * sizeof(JVal *));
    }
    a->arr[a->alen++] = v;
}

static JVal *obj_get_n(const JVal *o, const char *k, size_t n)
{
    if (!o || o->type != J_OBJ) return NULL;
    for (size_t i = 0; i < o->elen; i++) {
        if (o->ent[i].klen == n && (n == 0 || !memcmp(o->ent[i].k, k, n)))
            return o->ent[i].v;
    }
    return NULL;
}

static JVal *obj_get(const JVal *o, const char *k)
{
    return obj_get_n(o, k, strlen(k));
}

/* replace value for existing key, else append */
static void obj_set_n(JVal *o, const char *k, size_t n, JVal *v)
{
    for (size_t i = 0; i < o->elen; i++) {
        if (o->ent[i].klen == n && (n == 0 || !memcmp(o->ent[i].k, k, n))) {
            o->ent[i].v = v;
            return;
        }
    }
    if (o->elen == o->ecap) {
        o->ecap = o->ecap ? o->ecap * 2 : 4;
        o->ent = xrealloc(o->ent, o->ecap * sizeof(JEnt));
    }
    o->ent[o->elen].k = xstrndup(k, n);
    o->ent[o->elen].klen = n;
    o->ent[o->elen].v = v;
    o->elen++;
}

static void obj_set(JVal *o, const char *k, JVal *v)
{
    obj_set_n(o, k, strlen(k), v);
}

/* append without replacing; used by the parser so duplicate keys in the raw
 * document remain visible (first occurrence wins on lookup, and a few
 * operation-specific fallbacks can consult later occurrences). */
static void obj_append_n(JVal *o, const char *k, size_t n, JVal *v)
{
    if (o->elen == o->ecap) {
        o->ecap = o->ecap ? o->ecap * 2 : 4;
        o->ent = xrealloc(o->ent, o->ecap * sizeof(JEnt));
    }
    o->ent[o->elen].k = xstrndup(k, n);
    o->ent[o->elen].klen = n;
    o->ent[o->elen].v = v;
    o->elen++;
}

static const char *jtype_name(const JVal *v)
{
    switch (v->type) {
    case J_NULL:  return "null";
    case J_BOOL:  return "boolean";
    case J_NUM:   return "number";
    case J_STR:   return "string";
    case J_ARR:   return "array";
    case J_OBJ:   return "object";
    }
    return "null";
}

/* byte-wise lexicographic comparison of two byte strings */
static int bytes_cmp(const char *a, size_t an, const char *b, size_t bn)
{
    size_t m = an < bn ? an : bn;
    int c = m ? memcmp(a, b, m) : 0;
    if (c) return c < 0 ? -1 : 1;
    if (an == bn) return 0;
    return an < bn ? -1 : 1;
}

/* ------------------------------------------------------------------ */
/* JSON serialization (compact)                                        */
/* ------------------------------------------------------------------ */

static void wr_string(Buf *b, const char *s, size_t n)
{
    bputc(b, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  bputs(b, "\\\""); break;
        case '\\': bputs(b, "\\\\"); break;
        case '\b': bputs(b, "\\b");  break;
        case '\f': bputs(b, "\\f");  break;
        case '\n': bputs(b, "\\n");  break;
        case '\r': bputs(b, "\\r");  break;
        case '\t': bputs(b, "\\t");  break;
        default:
            if (c < 0x20) {
                char tmp[8];
                snprintf(tmp, sizeof tmp, "\\u%04x", (unsigned)c);
                bputs(b, tmp);
            } else {
                bputc(b, (char)c);
            }
        }
    }
    bputc(b, '"');
}

static void wr_val(Buf *b, const JVal *v);

static void wr_number(Buf *b, const JVal *v)
{
    if (v->is_int) {
        char tmp[32];
        snprintf(tmp, sizeof tmp, "%lld", v->i);
        bputs(b, tmp);
    } else if (v->s) {
        bwrite(b, v->s, v->slen);
    } else {
        char tmp[64];
        snprintf(tmp, sizeof tmp, "%.17g", v->d);
        bputs(b, tmp);
    }
}

static void wr_val(Buf *b, const JVal *v)
{
    switch (v->type) {
    case J_NULL: bputs(b, "null"); break;
    case J_BOOL: bputs(b, v->b ? "true" : "false"); break;
    case J_NUM:  wr_number(b, v); break;
    case J_STR:  wr_string(b, v->s, v->slen); break;
    case J_ARR:
        bputc(b, '[');
        for (size_t i = 0; i < v->alen; i++) {
            if (i) bputc(b, ',');
            wr_val(b, v->arr[i]);
        }
        bputc(b, ']');
        break;
    case J_OBJ:
        bputc(b, '{');
        for (size_t i = 0; i < v->elen; i++) {
            if (i) bputc(b, ',');
            wr_string(b, v->ent[i].k, v->ent[i].klen);
            bputc(b, ':');
            wr_val(b, v->ent[i].v);
        }
        bputc(b, '}');
        break;
    }
}

/* compact JSON text of a value, as an owned string */
static char *json_stringify(const JVal *v, size_t *outlen)
{
    Buf b = {0};
    bputc(&b, 0); /* ensure allocation */
    b.len = 0;
    if (b.p) b.p[0] = 0;
    wr_val(&b, v);
    if (outlen) *outlen = b.len;
    if (!b.p) b.p = xstrndup("", 0);
    return b.p;
}

/* ------------------------------------------------------------------ */
/* JSON parser                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *p, *end;
    int depth;
} Parser;

static void skip_ws(Parser *ps)
{
    while (ps->p < ps->end) {
        char c = *ps->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ps->p++;
        else break;
    }
}

static int parse_hex4(Parser *ps, unsigned *out)
{
    if (ps->end - ps->p < 4) return -1;
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = ps->p[i];
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else return -1;
        v = v * 16 + d;
    }
    ps->p += 4;
    *out = v;
    return 0;
}

/* parser->p points at opening quote; returns string value or NULL */
static JVal *parse_string(Parser *ps)
{
    ps->p++; /* opening quote */
    Buf b = {0};
    while (ps->p < ps->end) {
        unsigned char c = (unsigned char)*ps->p;
        if (c == '"') {
            ps->p++;
            if (!b.p) {
                b.p = xstrndup("", 0);
                b.cap = 1;
            }
            JVal *v = jnew(J_STR);
            v->s = b.p;
            v->slen = b.len;
            return v;
        }
        if (c == '\\') {
            ps->p++;
            if (ps->p >= ps->end) break;
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
                unsigned cp;
                if (parse_hex4(ps, &cp)) { free(b.p); return NULL; }
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    unsigned lo;
                    if (ps->end - ps->p < 6 || ps->p[0] != '\\' || ps->p[1] != 'u') {
                        free(b.p);
                        return NULL;
                    }
                    ps->p += 2;
                    if (parse_hex4(ps, &lo)) { free(b.p); return NULL; }
                    if (lo < 0xDC00 || lo > 0xDFFF) { free(b.p); return NULL; }
                    cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    free(b.p);
                    return NULL;
                }
                bput_utf8(&b, cp);
                break;
            }
            default:
                free(b.p);
                return NULL;
            }
        } else if (c < 0x20) {
            free(b.p);
            return NULL;
        } else {
            bputc(&b, (char)c);
            ps->p++;
        }
    }
    free(b.p);
    return NULL;
}

static int parse_i64(const char *s, size_t n, i64 *out)
{
    size_t i = 0;
    int neg = 0;
    if (i < n && s[i] == '-') { neg = 1; i++; }
    if (i >= n) return 0;
    unsigned long long v = 0;
    unsigned long long limit = neg ? 9223372036854775808ULL
                                   : 9223372036854775807ULL;
    for (; i < n; i++) {
        int d = (unsigned char)s[i] - '0';
        if (d < 0 || d > 9) return 0;
        if (v > (limit - (unsigned)d) / 10ULL) return 0;
        v = v * 10ULL + (unsigned)d;
    }
    if (neg) {
        *out = (v == 9223372036854775808ULL) ? (i64)INT64_MIN : -(i64)v;
    } else {
        *out = (i64)v;
    }
    return 1;
}

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static JVal *parse_number(Parser *ps)
{
    const char *start = ps->p;
    const char *q = ps->p;
    int frac = 0, exp = 0;
    if (q < ps->end && *q == '-') q++;
    if (q >= ps->end) return NULL;
    if (*q == '0') {
        q++;
    } else if (*q >= '1' && *q <= '9') {
        while (q < ps->end && is_digit(*q)) q++;
    } else {
        return NULL;
    }
    if (q < ps->end && *q == '.') {
        q++;
        if (q >= ps->end || !is_digit(*q)) return NULL;
        while (q < ps->end && is_digit(*q)) q++;
        frac = 1;
    }
    if (q < ps->end && (*q == 'e' || *q == 'E')) {
        q++;
        if (q < ps->end && (*q == '+' || *q == '-')) q++;
        if (q >= ps->end || !is_digit(*q)) return NULL;
        while (q < ps->end && is_digit(*q)) q++;
        exp = 1;
    }
    size_t len = (size_t)(q - start);
    JVal *v = jnew(J_NUM);
    v->s = xstrndup(start, len);
    v->slen = len;
    v->d = strtod(v->s, NULL);
    if (!frac && !exp) {
        i64 iv;
        if (parse_i64(start, len, &iv)) {
            v->is_int = 1;
            v->i = iv;
        }
    }
    ps->p = q;
    return v;
}

static int match_lit(Parser *ps, const char *lit)
{
    size_t n = strlen(lit);
    if ((size_t)(ps->end - ps->p) < n) return 0;
    if (memcmp(ps->p, lit, n)) return 0;
    ps->p += n;
    return 1;
}

static JVal *parse_value(Parser *ps)
{
    if (++ps->depth > 800) return NULL;
    skip_ws(ps);
    JVal *r = NULL;
    if (ps->p >= ps->end) goto done;
    char c = *ps->p;
    if (c == '{') {
        ps->p++;
        JVal *o = jobj();
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == '}') {
            ps->p++;
            r = o;
            goto done;
        }
        for (;;) {
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != '"') goto fail;
            JVal *k = parse_string(ps);
            if (!k) goto fail;
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') { free(k->s); free(k); goto fail; }
            ps->p++;
            JVal *val = parse_value(ps);
            if (!val) { free(k->s); free(k); goto fail; }
            obj_append_n(o, k->s, k->slen, val);
            free(k->s);
            free(k);
            skip_ws(ps);
            if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
            if (ps->p < ps->end && *ps->p == '}') { ps->p++; break; }
            goto fail;
        }
        r = o;
        goto done;
    fail:
        r = NULL;
        goto done;
    } else if (c == '[') {
        ps->p++;
        JVal *a = jarr();
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ']') {
            ps->p++;
            r = a;
            goto done;
        }
        for (;;) {
            JVal *val = parse_value(ps);
            if (!val) { r = NULL; goto done; }
            arr_push(a, val);
            skip_ws(ps);
            if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
            if (ps->p < ps->end && *ps->p == ']') { ps->p++; break; }
            r = NULL;
            goto done;
        }
        r = a;
        goto done;
    } else if (c == '"') {
        r = parse_string(ps);
        goto done;
    } else if (c == 't') {
        if (!match_lit(ps, "true")) goto done;
        r = jbool(1);
        goto done;
    } else if (c == 'f') {
        if (!match_lit(ps, "false")) goto done;
        r = jbool(0);
        goto done;
    } else if (c == 'n') {
        if (!match_lit(ps, "null")) goto done;
        r = jnull();
        goto done;
    } else if (c == '-' || is_digit(c)) {
        r = parse_number(ps);
        goto done;
    }
done:
    ps->depth--;
    return r;
}

/* ------------------------------------------------------------------ */
/* total order and structural equality                                 */
/* ------------------------------------------------------------------ */

static int type_rank(const JVal *v)
{
    switch (v->type) {
    case J_NUM:  return 0;
    case J_STR:  return 1;
    case J_BOOL: return 2;
    case J_NULL: return 3;
    case J_ARR:  return 4;
    case J_OBJ:  return 5;
    }
    return 6;
}

static int jcmp(const JVal *a, const JVal *b);

typedef struct {
    const char *k;
    size_t klen;
    JVal *v;
} KVs;

static int kv_key_cmp(const void *pa, const void *pb)
{
    const KVs *a = pa, *b = pb;
    return bytes_cmp(a->k, a->klen, b->k, b->klen);
}

static int jeq(const JVal *a, const JVal *b)
{
    if (a->type != b->type) return 0;
    switch (a->type) {
    case J_NULL: return 1;
    case J_BOOL: return a->b == b->b;
    case J_NUM:
        if (a->is_int && b->is_int) return a->i == b->i;
        return a->d == b->d;
    case J_STR:
        return a->slen == b->slen &&
               (a->slen == 0 || !memcmp(a->s, b->s, a->slen));
    case J_ARR:
        if (a->alen != b->alen) return 0;
        for (size_t i = 0; i < a->alen; i++)
            if (!jeq(a->arr[i], b->arr[i])) return 0;
        return 1;
    case J_OBJ: {
        if (a->elen != b->elen) return 0;
        for (size_t i = 0; i < a->elen; i++) {
            JVal *bv = obj_get_n(b, a->ent[i].k, a->ent[i].klen);
            if (!bv) return 0;
            if (!jeq(a->ent[i].v, bv)) return 0;
        }
        return 1;
    }
    }
    return 0;
}

static int num_cmp(const JVal *a, const JVal *b)
{
    if (a->is_int && b->is_int) {
        if (a->i < b->i) return -1;
        if (a->i > b->i) return 1;
        return 0;
    }
    double x = a->is_int ? (double)a->i : a->d;
    double y = b->is_int ? (double)b->i : b->d;
    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
}

static KVs *sorted_kvs(const JVal *o, size_t *n)
{
    KVs *kv = xmalloc((o->elen ? o->elen : 1) * sizeof(KVs));
    for (size_t i = 0; i < o->elen; i++) {
        kv[i].k = o->ent[i].k;
        kv[i].klen = o->ent[i].klen;
        kv[i].v = (JVal *)o->ent[i].v;
    }
    *n = o->elen;
    if (o->elen > 1) qsort(kv, o->elen, sizeof(KVs), kv_key_cmp);
    return kv;
}

static int jcmp(const JVal *a, const JVal *b)
{
    int ra = type_rank(a), rb = type_rank(b);
    if (ra != rb) return ra < rb ? -1 : 1;
    switch (a->type) {
    case J_NULL:
        return 0;
    case J_BOOL:
        if (a->b == b->b) return 0;
        return a->b < b->b ? -1 : 1;
    case J_NUM:
        return num_cmp(a, b);
    case J_STR:
        return bytes_cmp(a->s, a->slen, b->s, b->slen);
    case J_ARR: {
        size_t m = a->alen < b->alen ? a->alen : b->alen;
        for (size_t i = 0; i < m; i++) {
            int c = jcmp(a->arr[i], b->arr[i]);
            if (c) return c;
        }
        if (a->alen == b->alen) return 0;
        return a->alen < b->alen ? -1 : 1;
    }
    case J_OBJ: {
        size_t na, nb;
        KVs *ka = sorted_kvs(a, &na);
        KVs *kb = sorted_kvs(b, &nb);
        size_t m = na < nb ? na : nb;
        int result = 0;
        for (size_t i = 0; i < m; i++) {
            int c = bytes_cmp(ka[i].k, ka[i].klen, kb[i].k, kb[i].klen);
            if (c) { result = c; goto out; }
        }
        if (na != nb) { result = na < nb ? -1 : 1; goto out; }
        for (size_t i = 0; i < na; i++) {
            int c = jcmp(ka[i].v, kb[i].v);
            if (c) { result = c; goto out; }
        }
    out:
        free(ka);
        free(kb);
        return result;
    }
    }
    return 0;
}

static int jvalptr_cmp(const void *pa, const void *pb)
{
    const JVal *a = *(const JVal *const *)pa;
    const JVal *b = *(const JVal *const *)pb;
    return jcmp(a, b);
}

static int truthy(const JVal *v)
{
    switch (v->type) {
    case J_NULL: return 0;
    case J_BOOL: return v->b;
    case J_NUM:  return v->is_int ? v->i != 0 : v->d != 0.0;
    case J_STR:  return v->slen > 0;
    case J_ARR:  return v->alen > 0;
    case J_OBJ:  return v->elen > 0;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* errors, steps, context                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *code;   /* static string */
    const char *step;   /* step id or NULL; step ids live for program life */
    char msg[512];
    int exit_code;
} Err;

static int efail(Err *e, const char *code, const char *step, int exit_code,
                 const char *msg)
{
    e->code = code;
    e->step = step;
    e->exit_code = exit_code;
    snprintf(e->msg, sizeof e->msg, "%s", msg ? msg : "");
    return -1;
}

typedef struct {
    char *id;
    char *op;
    JVal *args;
    JVal *depv;        /* array of strings, or NULL */
    JVal *when;
    JVal *retryv;
    int max_attempts;
    int *dep_idx;
    int ndeps;
    int state;         /* 0 pending, 1 finished, 2 skipped */
    int attempts;
    JVal *result;
} Step;

typedef struct {
    Step *steps;
    int nsteps;
    JVal *results;
    JVal *order;
    JVal *skipped;
} Ctx;

static int find_step_n(Ctx *c, const char *id, size_t n)
{
    for (int i = 0; i < c->nsteps; i++) {
        size_t il = strlen(c->steps[i].id);
        if (il == n && (n == 0 || !memcmp(c->steps[i].id, id, n)))
            return i;
    }
    return -1;
}

static int is_dep(Ctx *c, int from, int target)
{
    if (from == target) return 0;
    unsigned char *vis = xcalloc((size_t)c->nsteps, 1);
    int *stack = xmalloc(sizeof(int) * (size_t)c->nsteps);
    int sp = 0;
    stack[sp++] = from;
    vis[from] = 1;
    int found = 0;
    while (sp) {
        int s = stack[--sp];
        Step *S = &c->steps[s];
        for (int i = 0; i < S->ndeps; i++) {
            int d = S->dep_idx[i];
            if (d == target) { found = 1; goto out; }
            if (!vis[d]) {
                vis[d] = 1;
                if (sp < c->nsteps) stack[sp++] = d;
            }
        }
    }
out:
    free(vis);
    free(stack);
    return found;
}

/* ------------------------------------------------------------------ */
/* references and conditions                                           */
/* ------------------------------------------------------------------ */

static int resolve_val(Ctx *c, int sidx, JVal *v, JVal **out, Err *e);

static int resolve_ref(Ctx *c, int sidx, JVal *pathv, JVal **out, Err *e)
{
    const char *path = pathv->s;
    size_t plen = pathv->slen;
    size_t dot = 0;
    while (dot < plen && path[dot] != '.') dot++;
    if (dot == 0)
        return efail(e, "INVALID_REFERENCE", c->steps[sidx].id, 1,
                     "empty reference target");
    int idx = find_step_n(c, path, dot);
    if (idx < 0)
        return efail(e, "INVALID_REFERENCE", c->steps[sidx].id, 1,
                     "reference to unknown step");
    if (!is_dep(c, sidx, idx))
        return efail(e, "INVALID_REFERENCE", c->steps[sidx].id, 1,
                     "reference to non-dependency");
    Step *t = &c->steps[idx];
    if (t->state == 2)
        return efail(e, "SKIPPED_REFERENCE", c->steps[sidx].id, 1,
                     "reference to skipped step");
    if (t->state != 1)
        return efail(e, "INVALID_REFERENCE", c->steps[sidx].id, 1,
                     "reference to unfinished step");
    JVal *cur = t->result;
    size_t pos = dot;
    while (pos < plen) {
        /* pos is at '.' */
        pos++;
        size_t st = pos;
        while (pos < plen && path[pos] != '.') pos++;
        size_t seglen = pos - st;
        if (cur->type == J_OBJ) {
            JVal *nx = obj_get_n(cur, path + st, seglen);
            if (!nx)
                return efail(e, "INVALID_REFERENCE", c->steps[sidx].id, 1,
                             "missing object key in reference");
            cur = nx;
        } else if (cur->type == J_ARR) {
            if (seglen == 0)
                return efail(e, "INVALID_REFERENCE", c->steps[sidx].id, 1,
                             "empty array index in reference");
            unsigned long long ix = 0;
            for (size_t i = 0; i < seglen; i++) {
                int d = (unsigned char)path[st + i] - '0';
                if (d < 0 || d > 9)
                    return efail(e, "INVALID_REFERENCE", c->steps[sidx].id, 1,
                                 "non-numeric array index in reference");
                if (ix > 100000000ULL)
                    return efail(e, "INVALID_REFERENCE", c->steps[sidx].id, 1,
                                 "array index out of range");
                ix = ix * 10ULL + (unsigned)d;
            }
            if (ix >= cur->alen)
                return efail(e, "INVALID_REFERENCE", c->steps[sidx].id, 1,
                             "array index out of range");
            cur = cur->arr[ix];
        } else {
            return efail(e, "INVALID_REFERENCE", c->steps[sidx].id, 1,
                         "cannot index non-container in reference");
        }
    }
    *out = cur;
    return 0;
}

static int resolve_val(Ctx *c, int sidx, JVal *v, JVal **out, Err *e)
{
    if (v->type == J_OBJ && v->elen == 1 &&
        v->ent[0].klen == 4 && !memcmp(v->ent[0].k, "$ref", 4) &&
        v->ent[0].v->type == J_STR) {
        return resolve_ref(c, sidx, v->ent[0].v, out, e);
    }
    if (v->type == J_ARR) {
        JVal *na = jarr();
        for (size_t i = 0; i < v->alen; i++) {
            JVal *x;
            if (resolve_val(c, sidx, v->arr[i], &x, e)) return -1;
            arr_push(na, x);
        }
        *out = na;
        return 0;
    }
    if (v->type == J_OBJ) {
        JVal *no = jobj();
        for (size_t i = 0; i < v->elen; i++) {
            JVal *x;
            if (resolve_val(c, sidx, v->ent[i].v, &x, e)) return -1;
            obj_append_n(no, v->ent[i].k, v->ent[i].klen, x);
        }
        *out = no;
        return 0;
    }
    *out = v;
    return 0;
}

static int cond_cmp(JVal *a, JVal *b, const char *op, int *out)
{
    int c;
    if (!strcmp(op, "eq")) { *out = jeq(a, b); return 0; }
    if (!strcmp(op, "ne")) { *out = !jeq(a, b); return 0; }
    c = jcmp(a, b);
    if (!strcmp(op, "gt")) *out = c > 0;
    else if (!strcmp(op, "gte")) *out = c >= 0;
    else if (!strcmp(op, "lt")) *out = c < 0;
    else *out = c <= 0;
    return 0;
}

static int eval_cond(Ctx *c, int sidx, JVal *node, int *out, Err *e)
{
    JVal *r;
    if (resolve_val(c, sidx, node, &r, e)) return -1;

    if (r->type == J_BOOL) {
        *out = r->b;
        return 0;
    }
    if (r->type != J_OBJ)
        return efail(e, "INVALID_CONDITION", c->steps[sidx].id, 1,
                     "condition is not a boolean");
    JVal *opv = obj_get(r, "op");
    if (!opv || opv->type != J_STR)
        return efail(e, "INVALID_CONDITION", c->steps[sidx].id, 1,
                     "condition has no operator");
    const char *op = opv->s;

    if (!strcmp(op, "all") || !strcmp(op, "any")) {
        int is_all = !strcmp(op, "all");
        JVal *cs = obj_get(r, "conditions");
        if (!cs || cs->type != J_ARR)
            return efail(e, "INVALID_CONDITION", c->steps[sidx].id, 1,
                         "conditions must be an array");
        *out = is_all ? 1 : 0;
        for (size_t i = 0; i < cs->alen; i++) {
            int b;
            if (eval_cond(c, sidx, cs->arr[i], &b, e)) return -1;
            if (is_all && !b) { *out = 0; return 0; }
            if (!is_all && b) { *out = 1; return 0; }
        }
        return 0;
    }
    if (!strcmp(op, "not")) {
        JVal *cn = obj_get(r, "condition");
        if (!cn)
            return efail(e, "INVALID_CONDITION", c->steps[sidx].id, 1,
                         "not requires a condition");
        int b;
        if (eval_cond(c, sidx, cn, &b, e)) return -1;
        *out = !b;
        return 0;
    }
    if (!strcmp(op, "eq") || !strcmp(op, "ne") || !strcmp(op, "gt") ||
        !strcmp(op, "gte") || !strcmp(op, "lt") || !strcmp(op, "lte")) {
        JVal *L = obj_get(r, "left");
        JVal *R = obj_get(r, "right");
        if (!L || !R)
            return efail(e, "INVALID_CONDITION", c->steps[sidx].id, 1,
                         "comparison condition missing operand");
        return cond_cmp(L, R, op, out);
    }
    return efail(e, "INVALID_CONDITION", c->steps[sidx].id, 1,
                 "unknown condition operator");
}

/* ------------------------------------------------------------------ */
/* operations                                                          */
/* ------------------------------------------------------------------ */

static int known_op(const char *op)
{
    static const char *names[] = {
        "add", "subtract", "multiply", "divide", "mod", "sum", "min", "max",
        "range", "concat", "split", "join", "uppercase", "lowercase", "trim",
        "replace", "length", "map", "filter", "reduce", "sort", "reverse",
        "unique", "slice", "flatten", "get", "set", "keys", "values", "merge",
        "pick", "omit", "constant", "equals", "compare", "and", "or", "not",
        "type", "fail_until", NULL
    };
    for (int i = 0; names[i]; i++)
        if (!strcmp(names[i], op)) return 1;
    return 0;
}

static int chk_unknown(JVal *args, const char *const *allowed, Err *e)
{
    for (size_t i = 0; i < args->elen; i++) {
        int ok = 0;
        for (const char *const *p = allowed; *p; p++) {
            size_t n = strlen(*p);
            if (n == args->ent[i].klen &&
                (n == 0 || !memcmp(*p, args->ent[i].k, n))) {
                ok = 1;
                break;
            }
        }
        if (!ok)
            return efail(e, "INVALID_ARG", NULL, 1, "unknown argument name");
    }
    return 0;
}

static int a_int(JVal *args, const char *name, i64 *out, Err *e)
{
    JVal *v = obj_get(args, name);
    if (!v)
        return efail(e, "INVALID_ARG", NULL, 1, "missing required argument");
    if (v->type != J_NUM || !v->is_int)
        return efail(e, "INVALID_ARG", NULL, 1,
                     "argument must be an integer");
    *out = v->i;
    return 0;
}

static int a_str(JVal *args, const char *name, JVal **out, Err *e)
{
    JVal *v = obj_get(args, name);
    if (!v)
        return efail(e, "INVALID_ARG", NULL, 1, "missing required argument");
    if (v->type != J_STR)
        return efail(e, "INVALID_ARG", NULL, 1,
                     "argument must be a string");
    *out = v;
    return 0;
}

static int a_arr(JVal *args, const char *name, JVal **out, Err *e)
{
    JVal *v = obj_get(args, name);
    if (!v)
        return efail(e, "INVALID_ARG", NULL, 1, "missing required argument");
    if (v->type != J_ARR)
        return efail(e, "INVALID_ARG", NULL, 1,
                     "argument must be an array");
    *out = v;
    return 0;
}

static int a_obj(JVal *args, const char *name, JVal **out, Err *e)
{
    JVal *v = obj_get(args, name);
    if (!v)
        return efail(e, "INVALID_ARG", NULL, 1, "missing required argument");
    if (v->type != J_OBJ)
        return efail(e, "INVALID_ARG", NULL, 1,
                     "argument must be an object");
    *out = v;
    return 0;
}

static int a_any(JVal *args, const char *name, JVal **out, Err *e)
{
    JVal *v = obj_get(args, name);
    if (!v)
        return efail(e, "INVALID_ARG", NULL, 1, "missing required argument");
    *out = v;
    return 0;
}

static int a_bool(JVal *args, const char *name, int *out, Err *e)
{
    JVal *v = obj_get(args, name);
    if (!v)
        return efail(e, "INVALID_ARG", NULL, 1, "missing required argument");
    if (v->type != J_BOOL)
        return efail(e, "INVALID_ARG", NULL, 1,
                     "argument must be a boolean");
    *out = v->b;
    return 0;
}

static int ovf_add(i64 a, i64 b, i64 *r)
{
    return __builtin_add_overflow(a, b, r);
}

static int ovf_sub(i64 a, i64 b, i64 *r)
{
    return __builtin_sub_overflow(a, b, r);
}

static int ovf_mul(i64 a, i64 b, i64 *r)
{
    return __builtin_mul_overflow(a, b, r);
}

static int apply_binop(const char *op, i64 a, i64 b, i64 *out, Err *e)
{
    if (!strcmp(op, "add")) {
        if (ovf_add(a, b, out))
            return efail(e, "ARITHMETIC_OVERFLOW", NULL, 1, "add overflow");
        return 0;
    }
    if (!strcmp(op, "subtract")) {
        if (ovf_sub(a, b, out))
            return efail(e, "ARITHMETIC_OVERFLOW", NULL, 1,
                         "subtract overflow");
        return 0;
    }
    if (!strcmp(op, "multiply")) {
        if (ovf_mul(a, b, out))
            return efail(e, "ARITHMETIC_OVERFLOW", NULL, 1,
                         "multiply overflow");
        return 0;
    }
    if (!strcmp(op, "divide") || !strcmp(op, "mod")) {
        if (b == 0)
            return efail(e, "DIVISION_BY_ZERO", NULL, 1,
                         "division by zero");
        if (a == (i64)INT64_MIN && b == -1)
            return efail(e, "ARITHMETIC_OVERFLOW", NULL, 1,
                         "division overflow");
        *out = !strcmp(op, "divide") ? a / b : a % b;
        return 0;
    }
    return efail(e, "INVALID_ARG", NULL, 1, "unsupported operation");
}

static int elem_int(const JVal *v, i64 *out, Err *e)
{
    if (v->type != J_NUM || !v->is_int)
        return efail(e, "INVALID_ARG", NULL, 1, "element is not an integer");
    *out = v->i;
    return 0;
}

static size_t find_sub(const char *h, size_t hn, const char *n, size_t nn,
                       size_t from)
{
    if (nn == 0 || hn < nn || from > hn - nn) return (size_t)-1;
    for (size_t i = from; i + nn <= hn; i++)
        if (!memcmp(h + i, n, nn)) return i;
    return (size_t)-1;
}

static JVal *json_quote_value(const JVal *v)
{
    size_t n;
    char *s = json_stringify(v, &n);
    JVal *r = jstr(s, n);
    free(s);
    return r;
}

/* map transform */
static int is_self_marker(const JVal *v)
{
    return v->type == J_OBJ && v->elen == 1 && v->ent[0].klen == 5 &&
           !memcmp(v->ent[0].k, "$self", 5);
}

static int apply_map_transform(JVal *t, JVal *elem, JVal **out, Err *e)
{
    if (t->type != J_OBJ)
        return efail(e, "INVALID_ARG", NULL, 1,
                     "transform must be an object");
    JVal *tv = obj_get(t, "type");
    if (!tv || tv->type != J_STR)
        return efail(e, "INVALID_ARG", NULL, 1,
                     "transform requires a type");
    const char *type = tv->s;

    if (!strcmp(type, "constant")) {
        JVal *val;
        if (a_any(t, "value", &val, e)) return -1;
        *out = val;
        return 0;
    }
    if (!strcmp(type, "negate")) {
        i64 x;
        if (elem_int(elem, &x, e)) return -1;
        if (x == (i64)INT64_MIN)
            return efail(e, "ARITHMETIC_OVERFLOW", NULL, 1,
                         "negation overflow");
        *out = jnum(-x);
        return 0;
    }
    if (!strcmp(type, "abs")) {
        i64 x;
        if (elem_int(elem, &x, e)) return -1;
        if (x == (i64)INT64_MIN)
            return efail(e, "ARITHMETIC_OVERFLOW", NULL, 1,
                         "absolute value overflow");
        *out = jnum(x < 0 ? -x : x);
        return 0;
    }
    if (!strcmp(type, "stringify")) {
        *out = json_quote_value(elem);
        return 0;
    }
    if (!strcmp(type, "type")) {
        *out = jstrz(jtype_name(elem));
        return 0;
    }
    if (!strcmp(type, "add") || !strcmp(type, "subtract") ||
        !strcmp(type, "multiply") || !strcmp(type, "divide") ||
        !strcmp(type, "mod")) {
        i64 x, y, r;
        JVal *vv;
        if (elem_int(elem, &x, e)) return -1;
        if (a_int(t, "value", &y, e)) return -1;
        if (apply_binop(type, x, y, &r, e)) return -1;
        vv = jnum(r);
        *out = vv;
        return 0;
    }
    if (!strcmp(type, "op")) {
        JVal *opv = obj_get(t, "op");
        if (!opv || opv->type != J_STR)
            return efail(e, "INVALID_ARG", NULL, 1,
                         "transform op requires an op name");
        const char *op = opv->s;
        if (strcmp(op, "add") && strcmp(op, "subtract") &&
            strcmp(op, "multiply") && strcmp(op, "divide") &&
            strcmp(op, "mod"))
            return efail(e, "INVALID_ARG", NULL, 1,
                         "unsupported transform op");
        JVal *targs;
        if (a_obj(t, "args", &targs, e)) return -1;
        JVal *av = obj_get(targs, "a");
        JVal *bv = obj_get(targs, "b");
        if (!av || !bv)
            return efail(e, "INVALID_ARG", NULL, 1,
                         "transform op requires a and b");
        int asf = is_self_marker(av), bsf = is_self_marker(bv);
        i64 x, y, r;
        if (!asf && !bsf)
            return efail(e, "INVALID_ARG", NULL, 1,
                         "transform op requires a self operand");
        if (asf) {
            if (elem_int(elem, &x, e)) return -1;
        } else {
            if (elem_int(av, &x, e)) return -1;
        }
        if (bsf) {
            if (elem_int(elem, &y, e)) return -1;
        } else {
            if (elem_int(bv, &y, e)) return -1;
        }
        if (apply_binop(op, x, y, &r, e)) return -1;
        *out = jnum(r);
        return 0;
    }
    return efail(e, "INVALID_ARG", NULL, 1, "unknown transform type");
}

/* filter predicate; returns 1/0 in *out */
static int apply_filter_predicate(JVal *p, JVal *elem, int *out, Err *e)
{
    if (p->type != J_OBJ)
        return efail(e, "INVALID_ARG", NULL, 1,
                     "predicate must be an object");
    JVal *tv = obj_get(p, "type");
    if (!tv || tv->type != J_STR)
        return efail(e, "INVALID_ARG", NULL, 1,
                     "predicate requires a type");
    const char *type = tv->s;

    if (!strcmp(type, "even") || !strcmp(type, "odd")) {
        i64 x;
        if (elem_int(elem, &x, e)) return -1;
        int is_even = (x % 2 == 0);
        *out = !strcmp(type, "even") ? is_even : !is_even;
        return 0;
    }
    if (!strcmp(type, "gt") || !strcmp(type, "gte") || !strcmp(type, "lt") ||
        !strcmp(type, "lte")) {
        JVal *val;
        if (a_any(p, "value", &val, e)) return -1;
        int c = jcmp(elem, val);
        if (!strcmp(type, "gt")) *out = c > 0;
        else if (!strcmp(type, "gte")) *out = c >= 0;
        else if (!strcmp(type, "lt")) *out = c < 0;
        else *out = c <= 0;
        return 0;
    }
    if (!strcmp(type, "eq") || !strcmp(type, "ne")) {
        JVal *val;
        if (a_any(p, "value", &val, e)) return -1;
        int b = jeq(elem, val);
        *out = !strcmp(type, "eq") ? b : !b;
        return 0;
    }
    if (!strcmp(type, "truthy") || !strcmp(type, "falsy")) {
        int b = truthy(elem);
        *out = !strcmp(type, "truthy") ? b : !b;
        return 0;
    }
    if (!strcmp(type, "type_is")) {
        /* SPEC writes the operand key as "type" too, which collides with the
         * predicate tag; "value" is accepted as the operand name, with
         * "type_name" as an alias. */
        JVal *val = obj_get(p, "value");
        if (!val) val = obj_get(p, "type_name");
        if (!val) {
            /* literal duplicate-key form from SPEC: {"type":"type_is",
             * "type":"<name>"}; the later "type" entry holds the operand. */
            for (size_t i = 0; i < p->elen; i++) {
                if (p->ent[i].klen == 4 &&
                    !memcmp(p->ent[i].k, "type", 4) &&
                    p->ent[i].v->type == J_STR) {
                    const char *nm = p->ent[i].v->s;
                    size_t nl = p->ent[i].v->slen;
                    if ((nl == 4 && !memcmp(nm, "null", 4)) ||
                        (nl == 7 && !memcmp(nm, "boolean", 7)) ||
                        (nl == 6 && !memcmp(nm, "number", 6)) ||
                        (nl == 6 && !memcmp(nm, "string", 6)) ||
                        (nl == 5 && !memcmp(nm, "array", 5)) ||
                        (nl == 6 && !memcmp(nm, "object", 6)))
                        val = p->ent[i].v;
                }
            }
        }
        if (!val)
            return efail(e, "INVALID_ARG", NULL, 1,
                         "type_is requires a type value");
        if (val->type != J_STR)
            return efail(e, "INVALID_ARG", NULL, 1,
                         "type_is requires a string type");
        const char *tn = jtype_name(elem);
        *out = val->slen == strlen(tn) && !memcmp(val->s, tn, val->slen);
        return 0;
    }
    if (!strcmp(type, "string_contains")) {
        JVal *val;
        if (a_any(p, "value", &val, e)) return -1;
        if (val->type != J_STR)
            return efail(e, "INVALID_ARG", NULL, 1,
                         "string_contains requires a string value");
        if (elem->type != J_STR)
            return efail(e, "INVALID_ARG", NULL, 1,
                         "string_contains element is not a string");
        *out = find_sub(elem->s, elem->slen, val->s, val->slen, 0) !=
               (size_t)-1;
        return 0;
    }
    return efail(e, "INVALID_ARG", NULL, 1, "unknown predicate type");
}

static void merge_sort_vals(JVal **a, JVal **tmp, size_t n)
{
    if (n < 2) return;
    size_t m = n / 2;
    merge_sort_vals(a, tmp, m);
    merge_sort_vals(a + m, tmp, n - m);
    size_t i = 0, j = m, k = 0;
    while (i < m && j < n) {
        if (jcmp(a[j], a[i]) < 0) tmp[k++] = a[j++];
        else tmp[k++] = a[i++];
    }
    while (i < m) tmp[k++] = a[i++];
    while (j < n) tmp[k++] = a[j++];
    memcpy(a, tmp, n * sizeof(JVal *));
}

static int run_op(Ctx *c, Step *s, JVal *args, JVal **out, Err *e)
{
    const char *op = s->op;
    (void)c;

    /* ---------------- math ---------------- */
    if (!strcmp(op, "add") || !strcmp(op, "subtract") ||
        !strcmp(op, "multiply") || !strcmp(op, "divide") ||
        !strcmp(op, "mod")) {
        static const char *const keys[] = {"a", "b", NULL};
        i64 a, b, r;
        if (chk_unknown(args, keys, e)) return -1;
        if (a_int(args, "a", &a, e)) return -1;
        if (a_int(args, "b", &b, e)) return -1;
        if (apply_binop(op, a, b, &r, e)) return -1;
        *out = jnum(r);
        return 0;
    }
    if (!strcmp(op, "sum")) {
        static const char *const keys[] = {"input", NULL};
        JVal *in;
        i64 acc = 0;
        if (chk_unknown(args, keys, e)) return -1;
        if (a_arr(args, "input", &in, e)) return -1;
        for (size_t i = 0; i < in->alen; i++) {
            i64 x;
            if (elem_int(in->arr[i], &x, e)) return -1;
            if (ovf_add(acc, x, &acc))
                return efail(e, "ARITHMETIC_OVERFLOW", NULL, 1,
                             "sum overflow");
        }
        *out = jnum(acc);
        return 0;
    }
    if (!strcmp(op, "min") || !strcmp(op, "max")) {
        static const char *const keys[] = {"input", NULL};
        JVal *in;
        if (chk_unknown(args, keys, e)) return -1;
        if (a_arr(args, "input", &in, e)) return -1;
        if (in->alen == 0)
            return efail(e, "EMPTY_ARRAY", NULL, 1,
                         "min/max of empty array");
        i64 m;
        if (elem_int(in->arr[0], &m, e)) return -1;
        for (size_t i = 1; i < in->alen; i++) {
            i64 x;
            if (elem_int(in->arr[i], &x, e)) return -1;
            if (!strcmp(op, "min") ? x < m : x > m) m = x;
        }
        *out = jnum(m);
        return 0;
    }
    if (!strcmp(op, "range")) {
        static const char *const keys[] = {"start", "end", NULL};
        i64 start, end;
        if (chk_unknown(args, keys, e)) return -1;
        if (a_int(args, "start", &start, e)) return -1;
        if (a_int(args, "end", &end, e)) return -1;
        JVal *a = jarr();
        if (start <= end) {
            for (i64 i = start;; i++) {
                arr_push(a, jnum(i));
                if (i == end) break;
            }
        }
        *out = a;
        return 0;
    }

    /* ---------------- strings ---------------- */
    if (!strcmp(op, "concat")) {
        static const char *const keys[] = {"a", "b", NULL};
        JVal *a, *b;
        if (chk_unknown(args, keys, e)) return -1;
        if (a_str(args, "a", &a, e)) return -1;
        if (a_str(args, "b", &b, e)) return -1;
        Buf buf = {0};
        bwrite(&buf, a->s, a->slen);
        bwrite(&buf, b->s, b->slen);
        JVal *r = jstr(buf.p ? buf.p : "", buf.len);
        free(buf.p);
        *out = r;
        return 0;
    }
    if (!strcmp(op, "split")) {
        static const char *const keys[] = {"input", "sep", NULL};
        JVal *in, *sep;
        if (chk_unknown(args, keys, e)) return -1;
        if (a_str(args, "input", &in, e)) return -1;
        if (a_str(args, "sep", &sep, e)) return -1;
        JVal *a = jarr();
        if (sep->slen == 0) {
            size_t i = 0;
            while (i < in->slen) {
                size_t j = i + 1;
                while (j < in->slen &&
                       (((unsigned char)in->s[j]) & 0xC0) == 0x80)
                    j++;
                arr_push(a, jstr(in->s + i, j - i));
                i = j;
            }
        } else {
            size_t i = 0;
            for (;;) {
                size_t pos = find_sub(in->s, in->slen, sep->s, sep->slen, i);
                if (pos == (size_t)-1) {
                    arr_push(a, jstr(in->s + i, in->slen - i));
                    break;
                }
                arr_push(a, jstr(in->s + i, pos - i));
                i = pos + sep->slen;
            }
        }
        *out = a;
        return 0;
    }
    if (!strcmp(op, "join")) {
        static const char *const keys[] = {"input", "sep", NULL};
        JVal *in, *sep;
        if (chk_unknown(args, keys, e)) return -1;
        if (a_arr(args, "input", &in, e)) return -1;
        if (a_str(args, "sep", &sep, e)) return -1;
        for (size_t i = 0; i < in->alen; i++)
            if (in->arr[i]->type != J_STR)
                return efail(e, "INVALID_ARG", NULL, 1,
                             "join element is not a string");
        Buf buf = {0};
        for (size_t i = 0; i < in->alen; i++) {
            if (i) bwrite(&buf, sep->s, sep->slen);
            bwrite(&buf, in->arr[i]->s, in->arr[i]->slen);
        }
        JVal *r = jstr(buf.p ? buf.p : "", buf.len);
        free(buf.p);
        *out = r;
        return 0;
    }
    if (!strcmp(op, "uppercase") || !strcmp(op, "lowercase")) {
        static const char *const keys[] = {"input", NULL};
        JVal *in;
        if (chk_unknown(args, keys, e)) return -1;
        if (a_str(args, "input", &in, e)) return -1;
        char *buf = xmalloc(in->slen + 1);
        int up = !strcmp(op, "uppercase");
        for (size_t i = 0; i < in->slen; i++) {
            char ch = in->s[i];
            if (up && ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
            else if (!up && ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
            buf[i] = ch;
        }
        JVal *r = jstr(buf, in->slen);
        free(buf);
        *out = r;
        return 0;
    }
    if (!strcmp(op, "trim")) {
        static const char *const keys[] = {"input", NULL};
        JVal *in;
        if (chk_unknown(args, keys, e)) return -1;
        if (a_str(args, "input", &in, e)) return -1;
        size_t i = 0, j = in->slen;
        while (i < j) {
            char ch = in->s[i];
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
                ch == '\f' || ch == '\v')
                i++;
            else
                break;
        }
        while (j > i) {
            char ch = in->s[j - 1];
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
                ch == '\f' || ch == '\v')
                j--;
            else
                break;
        }
        *out = jstr(in->s + i, j - i);
        return 0;
    }
    if (!strcmp(op, "replace")) {
        static const char *const keys[] = {"input", "search", "replace", NULL};
        JVal *in, *search, *repl;
        if (chk_unknown(args, keys, e)) return -1;
        if (a_str(args, "input", &in, e)) return -1;
        if (a_str(args, "search", &search, e)) return -1;
        if (a_str(args, "replace", &repl, e)) return -1;
        if (search->slen == 0)
            return efail(e, "INVALID_ARG", NULL, 1,
                         "replace search must not be empty");
        Buf buf = {0};
        size_t i = 0;
        for (;;) {
            size_t pos = find_sub(in->s, in->slen, search->s, search->slen, i);
            if (pos == (size_t)-1) {
                bwrite(&buf, in->s + i, in->slen - i);
                break;
            }
            bwrite(&buf, in->s + i, pos - i);
            bwrite(&buf, repl->s, repl->slen);
            i = pos + search->slen;
        }
        JVal *r = jstr(buf.p ? buf.p : "", buf.len);
        free(buf.p);
        *out = r;
        return 0;
    }
    if (!strcmp(op, "length")) {
        static const char *const keys[] = {"input", NULL};
        JVal *in;
        if (chk_unknown(args, keys, e)) return -1;
        if (a_str(args, "input", &in, e)) return -1;
        i64 n = 0;
        for (size_t i = 0; i < in->slen; i++)
            if ((((unsigned char)in->s[i]) & 0xC0) != 0x80) n++;
        *out = jnum(n);
        return 0;
    }

    /* ---------------- arrays ---------------- */
    if (!strcmp(op, "map")) {
        static const char *const keys[] = {"input", "transform", NULL};
        JVal *in, *tr;
        if (chk_unknown(args, keys, e)) return -1;
        if (a_arr(args, "input", &in, e)) return -1;
        if (a_obj(args, "transform", &tr, e)) return -1;
        JVal *res = jarr();
        for (size_t i = 0; i < in->alen; i++) {
            JVal *x;
            if (apply_map_transform(tr, in->arr[i], &x, e)) return -1;
            arr_push(res, x);
        }
        *out = res;
        return 0;
    }
    if (!strcmp(op, "filter")) {
        static const char *const keys[] = {"input", "predicate", NULL};
        JVal *in, *pr;
        if (chk_unknown(args, keys, e)) return -1;
        if (a_arr(args, "input", &in, e)) return -1;
        if (a_obj(args, "predicate", &pr, e)) return -1;
        JVal *res = jarr();
        for (size_t i = 0; i < in->alen; i++) {
            int keep;
            if (apply_filter_predicate(pr, in->arr[i], &keep, e)) return -1;
            if (keep) arr_push(res, in->arr[i]);
        }
        *out = res;
        return 0;
    }
    if (!strcmp(op, "reduce")) {
        static const char *const keys[] = {"input", "op", "initial", NULL};
        JVal *in, *opv;
        i64 acc;
        if (chk_unknown(args, keys, e)) return -1;
        if (a_arr(args, "input", &in, e)) return -1;
        if (a_str(args, "op", &opv, e)) return -1;
        const char *rop = opv->s;
        if (strcmp(rop, "add") && strcmp(rop, "subtract") &&
            strcmp(rop, "multiply"))
            return efail(e, "INVALID_ARG", NULL, 1,
                         "unsupported reduce operator");
        JVal *iv = obj_get(args, "initial");
        if (iv) {
            if (iv->type != J_NUM || !iv->is_int)
                return efail(e, "INVALID_ARG", NULL, 1,
                             "initial must be an integer");
            acc = iv->i;
        } else {
            acc = strcmp(rop, "multiply") ? 0 : 1;
        }
        for (size_t i = 0; i < in->alen; i++) {
            i64 x;
            if (elem_int(in->arr[i], &x, e)) return -1;
            if (!strcmp(rop, "add")) {
                if (ovf_add(acc, x, &acc))
                    return efail(e, "ARITHMETIC_OVERFLOW", NULL, 1,
                                 "reduce overflow");
            } else if (!strcmp(rop, "subtract")) {
                if (ovf_sub(acc, x, &acc))
                    return efail(e, "ARITHMETIC_OVERFLOW", NULL, 1,
                                 "reduce overflow");
            } else {
                if (ovf_mul(acc, x, &acc))
                    return efail(e, "ARITHMETIC_OVERFLOW", NULL, 1,
                                 "reduce overflow");
            }
        }
        *out = jnum(acc);
        return 0;
    }
    if (!strcmp(op, "sort")) {
        static const char *const keys[] = {"input", NULL};
        JVal *in;
        if (chk_unknown(args, keys, e)) return -1;
        if (a_arr(args, "input", &in, e)) return -1;
        JVal **items = xmalloc((in->alen ? in->alen : 1) * sizeof(JVal *));
        for (size_t i = 0; i < in->alen; i++) items[i] = in->arr[i];
        JVal **tmp = xmalloc((in->alen ? in->alen : 1) * sizeof(JVal *));
        merge_sort_vals(items, tmp, in->alen);
        JVal *res = jarr();
        for (size_t i = 0; i < in->alen; i++) arr_push(res, items[i]);
        free(items);
        free(tmp);
        *out = res;
        return 0;
    }
    if (!strcmp(op, "reverse")) {
        static const char *const keys[] = {"input", NULL};
        JVal *in;
        if (chk_unknown(args, keys, e)) return -1;
        if (a_arr(args, "input", &in, e)) return -1;
        JVal *res = jarr();
        for (size_t i = in->alen; i > 0; i--) arr_push(res, in->arr[i - 1]);
        *out = res;
        return 0;
    }
    if (!strcmp(op, "unique")) {
        static const char *const keys[] = {"input", NULL};
        JVal *in;
        if (chk_unknown(args, keys, e)) return -1;
        if (a_arr(args, "input", &in, e)) return -1;
        JVal *res = jarr();
        for (size_t i = 0; i < in->alen; i++) {
            int dup = 0;
            for (size_t j = 0; j < res->alen; j++)
                if (jeq(res->arr[j], in->arr[i])) { dup = 1; break; }
            if (!dup) arr_push(res, in->arr[i]);
        }
        *out = res;
        return 0;
    }
    if (!strcmp(op, "slice")) {
        static const char *const keys[] = {"input", "start", "end", NULL};
        JVal *in;
        i64 start, end;
        int have_end = 0;
        if (chk_unknown(args, keys, e)) return -1;
        if (a_arr(args, "input", &in, e)) return -1;
        if (a_int(args, "start", &start, e)) return -1;
        JVal *ev = obj_get(args, "end");
        if (ev) {
            if (ev->type != J_NUM || !ev->is_int)
                return efail(e, "INVALID_ARG", NULL, 1,
                             "end must be an integer");
            end = ev->i;
            have_end = 1;
        } else {
            end = 0;
        }
        i64 len = (i64)in->alen;
        if (start < 0) {
            if (start < -len) start = 0;
            else start += len;
        } else if (start > len) {
            start = len;
        }
        if (!have_end) {
            end = len;
        } else if (end < 0) {
            if (end < -len) end = 0;
            else end += len;
        } else if (end > len) {
            end = len;
        }
        JVal *res = jarr();
        for (i64 i = start; i < end; i++) arr_push(res, in->arr[i]);
        *out = res;
        return 0;
    }
    if (!strcmp(op, "flatten")) {
        static const char *const keys[] = {"input", NULL};
        JVal *in;
        if (chk_unknown(args, keys, e)) return -1;
        if (a_arr(args, "input", &in, e)) return -1;
        JVal *res = jarr();
        for (size_t i = 0; i < in->alen; i++) {
            JVal *e = in->arr[i];
            if (e->type == J_ARR) {
                for (size_t j = 0; j < e->alen; j++) arr_push(res, e->arr[j]);
            } else {
                arr_push(res, e);
            }
        }
        *out = res;
        return 0;
    }

    /* ---------------- objects ---------------- */
    if (!strcmp(op, "get")) {
        static const char *const keys[] = {"input", "key", NULL};
        JVal *in, *key;
        if (chk_unknown(args, keys, e)) return -1;
        if (a_obj(args, "input", &in, e)) return -1;
        if (a_str(args, "key", &key, e)) return -1;
        JVal *v = obj_get_n(in, key->s, key->slen);
        *out = v ? v : jnull();
        return 0;
    }
    if (!strcmp(op, "set")) {
        static const char *const keys[] = {"input", "key", "value", NULL};
        JVal *in, *key, *val;
        if (chk_unknown(args, keys, e)) return -1;
        if (a_obj(args, "input", &in, e)) return -1;
        if (a_str(args, "key", &key, e)) return -1;
        if (a_any(args, "value", &val, e)) return -1;
        JVal *res = jobj();
        for (size_t i = 0; i < in->elen; i++)
            obj_set_n(res, in->ent[i].k, in->ent[i].klen, in->ent[i].v);
        obj_set_n(res, key->s, key->slen, val);
        *out = res;
        return 0;
    }
    if (!strcmp(op, "keys") || !strcmp(op, "values")) {
        static const char *const keys[] = {"input", NULL};
        JVal *in;
        if (chk_unknown(args, keys, e)) return -1;
        if (a_obj(args, "input", &in, e)) return -1;
        size_t n;
        KVs *kv = sorted_kvs(in, &n);
        JVal *res = jarr();
        int want_keys = !strcmp(op, "keys");
        for (size_t i = 0; i < n; i++)
            arr_push(res, want_keys ? jstr(kv[i].k, kv[i].klen) : kv[i].v);
        free(kv);
        *out = res;
        return 0;
    }
    if (!strcmp(op, "merge")) {
        static const char *const keys[] = {"a", "b", NULL};
        JVal *a, *b;
        if (chk_unknown(args, keys, e)) return -1;
        if (a_obj(args, "a", &a, e)) return -1;
        if (a_obj(args, "b", &b, e)) return -1;
        JVal *res = jobj();
        for (size_t i = 0; i < a->elen; i++)
            obj_set_n(res, a->ent[i].k, a->ent[i].klen, a->ent[i].v);
        for (size_t i = 0; i < b->elen; i++)
            obj_set_n(res, b->ent[i].k, b->ent[i].klen, b->ent[i].v);
        *out = res;
        return 0;
    }
    if (!strcmp(op, "pick")) {
        static const char *const keys[] = {"input", "keys", NULL};
        JVal *in, *ks;
        if (chk_unknown(args, keys, e)) return -1;
        if (a_obj(args, "input", &in, e)) return -1;
        if (a_arr(args, "keys", &ks, e)) return -1;
        for (size_t i = 0; i < ks->alen; i++)
            if (ks->arr[i]->type != J_STR)
                return efail(e, "INVALID_ARG", NULL, 1,
                             "pick keys must be strings");
        JVal **sorted = xmalloc((ks->alen ? ks->alen : 1) * sizeof(JVal *));
        for (size_t i = 0; i < ks->alen; i++) sorted[i] = ks->arr[i];
        if (ks->alen > 1)
            qsort(sorted, ks->alen, sizeof(JVal *), jvalptr_cmp);
        JVal *res = jobj();
        for (size_t i = 0; i < ks->alen; i++) {
            if (i > 0 && jeq(sorted[i - 1], sorted[i])) continue;
            JVal *v = obj_get_n(in, sorted[i]->s, sorted[i]->slen);
            obj_set_n(res, sorted[i]->s, sorted[i]->slen, v ? v : jnull());
        }
        free(sorted);
        *out = res;
        return 0;
    }
    if (!strcmp(op, "omit")) {
        static const char *const keys[] = {"input", "keys", NULL};
        JVal *in, *ks;
        if (chk_unknown(args, keys, e)) return -1;
        if (a_obj(args, "input", &in, e)) return -1;
        if (a_arr(args, "keys", &ks, e)) return -1;
        for (size_t i = 0; i < ks->alen; i++)
            if (ks->arr[i]->type != J_STR)
                return efail(e, "INVALID_ARG", NULL, 1,
                             "omit keys must be strings");
        JVal *res = jobj();
        for (size_t i = 0; i < in->elen; i++) {
            int skip = 0;
            for (size_t j = 0; j < ks->alen; j++) {
                if (ks->arr[j]->slen == in->ent[i].klen &&
                    (ks->arr[j]->slen == 0 ||
                     !memcmp(ks->arr[j]->s, in->ent[i].k, in->ent[i].klen))) {
                    skip = 1;
                    break;
                }
            }
            if (!skip)
                obj_set_n(res, in->ent[i].k, in->ent[i].klen,
                          in->ent[i].v);
        }
        *out = res;
        return 0;
    }

    /* ---------------- utilities ---------------- */
    if (!strcmp(op, "constant")) {
        static const char *const keys[] = {"value", NULL};
        JVal *v;
        if (chk_unknown(args, keys, e)) return -1;
        if (a_any(args, "value", &v, e)) return -1;
        *out = v;
        return 0;
    }
    if (!strcmp(op, "equals")) {
        static const char *const keys[] = {"a", "b", NULL};
        JVal *a, *b;
        if (chk_unknown(args, keys, e)) return -1;
        if (a_any(args, "a", &a, e)) return -1;
        if (a_any(args, "b", &b, e)) return -1;
        *out = jbool(jeq(a, b));
        return 0;
    }
    if (!strcmp(op, "compare")) {
        static const char *const keys[] = {"a", "b", "op", NULL};
        JVal *a, *b, *opv;
        if (chk_unknown(args, keys, e)) return -1;
        if (a_any(args, "a", &a, e)) return -1;
        if (a_any(args, "b", &b, e)) return -1;
        if (a_str(args, "op", &opv, e)) return -1;
        const char *cop = opv->s;
        if (strcmp(cop, "gt") && strcmp(cop, "gte") && strcmp(cop, "lt") &&
            strcmp(cop, "lte"))
            return efail(e, "INVALID_ARG", NULL, 1,
                         "invalid compare operator");
        int cc = jcmp(a, b);
        int res;
        if (!strcmp(cop, "gt")) res = cc > 0;
        else if (!strcmp(cop, "gte")) res = cc >= 0;
        else if (!strcmp(cop, "lt")) res = cc < 0;
        else res = cc <= 0;
        *out = jbool(res);
        return 0;
    }
    if (!strcmp(op, "and") || !strcmp(op, "or")) {
        static const char *const keys[] = {"a", "b", NULL};
        int a, b;
        if (chk_unknown(args, keys, e)) return -1;
        if (a_bool(args, "a", &a, e)) return -1;
        if (a_bool(args, "b", &b, e)) return -1;
        *out = jbool(!strcmp(op, "and") ? (a && b) : (a || b));
        return 0;
    }
    if (!strcmp(op, "not")) {
        static const char *const keys[] = {"a", NULL};
        int a;
        if (chk_unknown(args, keys, e)) return -1;
        if (a_bool(args, "a", &a, e)) return -1;
        *out = jbool(!a);
        return 0;
    }
    if (!strcmp(op, "type")) {
        static const char *const keys[] = {"value", NULL};
        JVal *v;
        if (chk_unknown(args, keys, e)) return -1;
        if (a_any(args, "value", &v, e)) return -1;
        *out = jstrz(jtype_name(v));
        return 0;
    }
    if (!strcmp(op, "fail_until")) {
        static const char *const keys[] = {"failures", "value", NULL};
        i64 failures;
        JVal *v;
        if (chk_unknown(args, keys, e)) return -1;
        if (a_int(args, "failures", &failures, e)) return -1;
        if (failures < 0 || failures > 10)
            return efail(e, "INVALID_ARG", NULL, 1,
                         "failures must be between 0 and 10");
        if (a_any(args, "value", &v, e)) return -1;
        if ((i64)s->attempts <= failures)
            return efail(e, "TRANSIENT_FAILURE", NULL, 1,
                         "transient failure");
        *out = v;
        return 0;
    }

    return efail(e, "INVALID_WORKFLOW", NULL, 2, "unknown operation");
}

/* ------------------------------------------------------------------ */
/* structural validation                                               */
/* ------------------------------------------------------------------ */

static int valid_id_string(const char *s, size_t n)
{
    if (n == 0) return 0;
    char c = s[0];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'))
        return 0;
    for (size_t i = 1; i < n; i++) {
        c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_'))
            return 0;
    }
    return 1;
}

static int validate_cond_shape(JVal *w)
{
    if (w->type != J_OBJ) return -1;
    JVal *opv = obj_get(w, "op");
    if (!opv) return 0; /* defer to runtime (may be a reference) */
    if (opv->type != J_STR) return -1;
    const char *op = opv->s;
    if (!strcmp(op, "all") || !strcmp(op, "any")) {
        JVal *cs = obj_get(w, "conditions");
        if (!cs || cs->type != J_ARR) return -1;
        for (size_t i = 0; i < cs->alen; i++) {
            JVal *cc = cs->arr[i];
            if (cc->type == J_OBJ) {
                if (validate_cond_shape(cc)) return -1;
            }
            /* non-object sub-node deferred to runtime */
        }
        return 0;
    }
    if (!strcmp(op, "not")) {
        JVal *cc = obj_get(w, "condition");
        if (!cc) return -1;
        if (cc->type == J_OBJ) return validate_cond_shape(cc);
        return 0;
    }
    if (!strcmp(op, "eq") || !strcmp(op, "ne") || !strcmp(op, "gt") ||
        !strcmp(op, "gte") || !strcmp(op, "lt") || !strcmp(op, "lte")) {
        if (!obj_get(w, "left") || !obj_get(w, "right")) return -1;
        return 0;
    }
    return -1;
}

static int validate_step(Ctx *c, int i, JVal *sv, Err *e)
{
    if (sv->type != J_OBJ)
        return efail(e, "INVALID_WORKFLOW", NULL, 2,
                     "step must be an object");
    JVal *idv = obj_get(sv, "id");
    if (!idv || idv->type != J_STR)
        return efail(e, "INVALID_WORKFLOW", NULL, 2,
                     "step id must be a string");
    char *id = xstrndup(idv->s, idv->slen);
    c->steps[i].id = id;
    const char *stepref = c->steps[i].id;
    if (!valid_id_string(idv->s, idv->slen))
        return efail(e, "INVALID_WORKFLOW", stepref, 2,
                     "invalid step id");

    for (int k = 0; k < i; k++) {
        if (!strcmp(c->steps[k].id, id))
            return efail(e, "INVALID_WORKFLOW", stepref, 2,
                         "duplicate step id");
    }

    static const char *const allowed[] = {
        "id", "op", "args", "depends_on", "when", "retry", NULL
    };
    for (size_t k = 0; k < sv->elen; k++) {
        int ok = 0;
        for (const char *const *p = allowed; *p; p++) {
            size_t n = strlen(*p);
            if (n == sv->ent[k].klen &&
                (n == 0 || !memcmp(*p, sv->ent[k].k, n))) {
                ok = 1;
                break;
            }
        }
        if (!ok)
            return efail(e, "INVALID_WORKFLOW", stepref, 2,
                         "unknown step field");
    }

    JVal *opv = obj_get(sv, "op");
    if (!opv || opv->type != J_STR)
        return efail(e, "INVALID_WORKFLOW", stepref, 2,
                     "step op must be a string");
    char *opname = xstrndup(opv->s, opv->slen);
    c->steps[i].op = opname;
    if (!known_op(opname))
        return efail(e, "INVALID_WORKFLOW", stepref, 2, "unknown op");

    JVal *args = obj_get(sv, "args");
    if (args) {
        if (args->type != J_OBJ)
            return efail(e, "INVALID_WORKFLOW", stepref, 2,
                         "args must be an object");
        c->steps[i].args = args;
    } else {
        c->steps[i].args = jobj();
    }

    JVal *dep = obj_get(sv, "depends_on");
    if (dep) {
        if (dep->type != J_ARR)
            return efail(e, "INVALID_WORKFLOW", stepref, 2,
                         "depends_on must be an array");
        for (size_t k = 0; k < dep->alen; k++) {
            if (dep->arr[k]->type != J_STR)
                return efail(e, "INVALID_WORKFLOW", stepref, 2,
                             "depends_on entries must be strings");
        }
        c->steps[i].depv = dep;
    }

    c->steps[i].when = obj_get(sv, "when");
    c->steps[i].retryv = obj_get(sv, "retry");
    c->steps[i].max_attempts = 1;
    return 0;
}

/* retry / when are validated after dependency existence and cycle checks,
 * following the ordering listed in SPEC section 7. */
static int validate_step_deferred(Ctx *c, int i, Err *e)
{
    Step *s = &c->steps[i];
    JVal *retry = s->retryv;
    if (retry) {
        if (retry->type != J_OBJ)
            return efail(e, "INVALID_WORKFLOW", s->id, 2,
                         "retry must be an object");
        for (size_t k = 0; k < retry->elen; k++) {
            if (!(retry->ent[k].klen == 12 &&
                  !memcmp(retry->ent[k].k, "max_attempts", 12)))
                return efail(e, "INVALID_WORKFLOW", s->id, 2,
                             "unknown retry field");
        }
        JVal *ma = obj_get(retry, "max_attempts");
        if (!ma || ma->type != J_NUM || !ma->is_int)
            return efail(e, "INVALID_WORKFLOW", s->id, 2,
                         "retry max_attempts must be an integer");
        if (ma->i < 1 || ma->i > 5)
            return efail(e, "INVALID_WORKFLOW", s->id, 2,
                         "retry max_attempts out of range");
        s->max_attempts = (int)ma->i;
    }
    return 0;
}

static int validate_step_when(Ctx *c, int i, Err *e)
{
    Step *s = &c->steps[i];
    if (s->when && validate_cond_shape(s->when))
        return efail(e, "INVALID_WORKFLOW", s->id, 2,
                     "invalid when condition");
    return 0;
}

static int validate_workflow(Ctx *c, JVal *doc, Err *e)
{
    if (doc->type != J_OBJ)
        return efail(e, "INVALID_WORKFLOW", NULL, 2,
                     "workflow must be an object");
    for (size_t i = 0; i < doc->elen; i++) {
        int ok = 0;
        if (doc->ent[i].klen == 7 && !memcmp(doc->ent[i].k, "version", 7))
            ok = 1;
        if (doc->ent[i].klen == 5 && !memcmp(doc->ent[i].k, "steps", 5))
            ok = 1;
        if (!ok)
            return efail(e, "INVALID_WORKFLOW", NULL, 2,
                         "unknown top-level field");
    }
    JVal *ver = obj_get(doc, "version");
    if (!ver)
        return efail(e, "INVALID_WORKFLOW", NULL, 2, "missing version");
    if (ver->type != J_NUM || !ver->is_int || ver->i != 1)
        return efail(e, "INVALID_WORKFLOW", NULL, 2,
                     "version must be the integer 1");
    JVal *steps = obj_get(doc, "steps");
    if (!steps || steps->type != J_ARR)
        return efail(e, "INVALID_WORKFLOW", NULL, 2,
                     "steps must be an array");

    c->nsteps = (int)steps->alen;
    c->steps = xcalloc((size_t)c->nsteps, sizeof(Step));

    for (int i = 0; i < c->nsteps; i++) {
        if (validate_step(c, i, steps->arr[i], e)) return -1;
    }

    /* dependency existence + index arrays */
    for (int i = 0; i < c->nsteps; i++) {
        Step *s = &c->steps[i];
        if (!s->depv) {
            s->ndeps = 0;
            s->dep_idx = NULL;
            continue;
        }
        int n = (int)s->depv->alen;
        s->dep_idx = xmalloc(sizeof(int) * (size_t)(n ? n : 1));
        s->ndeps = 0;
        for (int k = 0; k < n; k++) {
            JVal *dv = s->depv->arr[k];
            int idx = find_step_n(c, dv->s, dv->slen);
            if (idx < 0)
                return efail(e, "UNKNOWN_DEPENDENCY", s->id, 2,
                             "depends_on names an unknown step");
            s->dep_idx[s->ndeps++] = idx;
        }
    }

    /* cycle detection: Kahn's algorithm, O(nodes + edges) */
    {
        int n = c->nsteps;
        int *indeg = xcalloc((size_t)(n ? n : 1), sizeof(int));
        int *child_cnt = xcalloc((size_t)(n ? n : 1), sizeof(int));
        int total_edges = 0;
        for (int i = 0; i < n; i++) {
            Step *s = &c->steps[i];
            indeg[i] += s->ndeps; /* duplicate edges counted consistently */
            total_edges += s->ndeps;
            for (int k = 0; k < s->ndeps; k++)
                child_cnt[s->dep_idx[k]]++;
        }
        int *child_start = xcalloc((size_t)(n ? n : 1), sizeof(int));
        for (int i = 1; i < n; i++)
            child_start[i] = child_start[i - 1] + child_cnt[i - 1];
        int *fill = xmalloc(sizeof(int) * (size_t)(n ? n : 1));
        memcpy(fill, child_start, sizeof(int) * (size_t)(n ? n : 1));
        int *children = xmalloc(sizeof(int) *
                                (size_t)(total_edges ? total_edges : 1));
        for (int i = 0; i < n; i++) {
            Step *s = &c->steps[i];
            for (int k = 0; k < s->ndeps; k++)
                children[fill[s->dep_idx[k]]++] = i;
        }
        int *queue = xmalloc(sizeof(int) * (size_t)(n ? n : 1));
        int head = 0, tail = 0, processed = 0;
        for (int i = 0; i < n; i++)
            if (indeg[i] == 0) queue[tail++] = i;
        while (head < tail) {
            int u = queue[head++];
            processed++;
            for (int k = child_start[u]; k < child_start[u] + child_cnt[u]; k++) {
                int v = children[k];
                if (--indeg[v] == 0) queue[tail++] = v;
            }
        }
        free(indeg);
        free(child_cnt);
        free(child_start);
        free(fill);
        free(children);
        free(queue);
        if (processed < n)
            return efail(e, "CYCLIC_DEPENDENCY", NULL, 2,
                         "workflow contains a dependency cycle");
    }

    for (int i = 0; i < c->nsteps; i++)
        if (validate_step_deferred(c, i, e)) return -1;
    for (int i = 0; i < c->nsteps; i++)
        if (validate_step_when(c, i, e)) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* execution                                                           */
/* ------------------------------------------------------------------ */

static JVal *jstr_copy(const char *s)
{
    return jstrz(s);
}

static int execute(Ctx *c, Err *e)
{
    unsigned char *done = xcalloc((size_t)c->nsteps, 1);
    JVal *empty_args = jobj();

    for (int k = 0; k < c->nsteps; k++) {
        int pick = -1;
        for (int i = 0; i < c->nsteps; i++) {
            if (done[i]) continue;
            int ready = 1;
            for (int j = 0; j < c->steps[i].ndeps; j++) {
                if (!done[c->steps[i].dep_idx[j]]) { ready = 0; break; }
            }
            if (ready) { pick = i; break; }
        }
        if (pick < 0) {
            free(done);
            return efail(e, "INVALID_WORKFLOW", NULL, 3,
                         "internal scheduling failure");
        }
        Step *s = &c->steps[pick];
        s->attempts = 0;

        if (s->when) {
            int cond = 0;
            if (eval_cond(c, pick, s->when, &cond, e)) {
                free(done);
                return -1;
            }
            if (!cond) {
                s->state = 2;
                s->result = jnull();
                obj_set(c->results, s->id, jnull());
                arr_push(c->order, jstr_copy(s->id));
                arr_push(c->skipped, jstr_copy(s->id));
                done[pick] = 1;
                continue;
            }
        }

        JVal *rargs;
        if (resolve_val(c, pick, s->args ? s->args : empty_args, &rargs, e)) {
            free(done);
            return -1;
        }

        JVal *val = NULL;
        int success = 0;
        for (int att = 1; att <= s->max_attempts; att++) {
            Err e2;
            memset(&e2, 0, sizeof e2);
            s->attempts++;
            val = NULL;
            if (run_op(c, s, rargs, &val, &e2) == 0) {
                success = 1;
                break;
            }
            if (!strcmp(e2.code, "TRANSIENT_FAILURE") &&
                att < s->max_attempts)
                continue;
            /* definitive failure */
            const char *code = e2.code ? e2.code : "STEP_FAILED";
            if (!strcmp(code, "TRANSIENT_FAILURE")) code = "STEP_FAILED";
            efail(e, code, s->id, 1, e2.msg);
            free(done);
            return -1;
        }
        if (!success) {
            efail(e, "STEP_FAILED", s->id, 1, "step failed");
            free(done);
            return -1;
        }
        s->state = 1;
        s->result = val;
        obj_set(c->results, s->id, val);
        arr_push(c->order, jstr_copy(s->id));
        done[pick] = 1;
    }
    free(done);
    return 0;
}

/* ------------------------------------------------------------------ */
/* response / main                                                     */
/* ------------------------------------------------------------------ */

static void write_response(Buf *b, const char *status, JVal *results,
                           JVal *order, JVal *skipped, Err *err)
{
    bputs(b, "{\"status\":");
    wr_string(b, status, strlen(status));
    bputs(b, ",\"results\":");
    wr_val(b, results);
    bputs(b, ",\"execution_order\":");
    wr_val(b, order);
    bputs(b, ",\"skipped\":");
    wr_val(b, skipped);
    bputs(b, ",\"error\":");
    if (!err) {
        bputs(b, "null");
    } else {
        bputs(b, "{\"code\":");
        wr_string(b, err->code ? err->code : "STEP_FAILED",
                  strlen(err->code ? err->code : "STEP_FAILED"));
        bputs(b, ",\"step\":");
        if (err->step) wr_string(b, err->step, strlen(err->step));
        else bputs(b, "null");
        bputs(b, ",\"message\":");
        wr_string(b, err->msg, strlen(err->msg));
        bputs(b, "}");
    }
    bputs(b, "}");
}

static char *read_stream(FILE *f, size_t *outlen)
{
    size_t cap = 65536, len = 0;
    char *buf = xmalloc(cap);
    for (;;) {
        if (len == cap) {
            cap *= 2;
            buf = xrealloc(buf, cap);
        }
        size_t r = fread(buf + len, 1, cap - len, f);
        if (r == 0) {
            if (ferror(f)) { free(buf); return NULL; }
            break;
        }
        len += r;
    }
    buf = xrealloc(buf, len + 1);
    buf[len] = 0;
    if (outlen) *outlen = len;
    return buf;
}

int main(int argc, char **argv)
{
    Err err;
    memset(&err, 0, sizeof err);
    Buf out = {0};
    JVal *results = jobj();
    JVal *order = jarr();
    JVal *skipped = jarr();
    Ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.results = results;
    ctx.order = order;
    ctx.skipped = skipped;

    char *data = NULL;
    size_t datalen = 0;

    if (argc != 3 || strcmp(argv[1], "run") != 0) {
        efail(&err, "INVALID_WORKFLOW", NULL, 2,
              "usage: workflow run <workflow.json>|-");
        goto respond;
    }

    if (!strcmp(argv[2], "-")) {
        data = read_stream(stdin, &datalen);
        if (!data) {
            efail(&err, "INVALID_WORKFLOW", NULL, 2,
                  "could not read stdin");
            goto respond;
        }
    } else {
        FILE *f = fopen(argv[2], "rb");
        if (!f) {
            efail(&err, "INVALID_WORKFLOW", NULL, 2,
                  "could not open workflow file");
            goto respond;
        }
        data = read_stream(f, &datalen);
        fclose(f);
        if (!data) {
            efail(&err, "INVALID_WORKFLOW", NULL, 2,
                  "could not read workflow file");
            goto respond;
        }
    }

    {
        Parser ps;
        ps.p = data;
        ps.end = data + datalen;
        ps.depth = 0;
        skip_ws(&ps);
        JVal *doc = parse_value(&ps);
        if (!doc) {
            efail(&err, "INVALID_WORKFLOW", NULL, 2, "invalid JSON");
            goto respond;
        }
        skip_ws(&ps);
        if (ps.p != ps.end) {
            efail(&err, "INVALID_WORKFLOW", NULL, 2,
                  "trailing content after JSON document");
            goto respond;
        }
        if (validate_workflow(&ctx, doc, &err)) goto respond;
        if (execute(&ctx, &err)) goto respond;
    }

    write_response(&out, "completed", results, order, skipped, NULL);
    fwrite(out.p, 1, out.len, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    return 0;

respond:
    write_response(&out, "failed", results, order, skipped, &err);
    fwrite(out.p, 1, out.len, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    if (err.exit_code == 2) return 2;
    if (err.exit_code == 3) return 3;
    return 1;
}
