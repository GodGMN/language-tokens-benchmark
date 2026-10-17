#include "json.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ */
/* allocation                                                          */
/* ------------------------------------------------------------------ */

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "workflow: out of memory\n");
        exit(3);
    }
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fprintf(stderr, "workflow: out of memory\n");
        exit(3);
    }
    return q;
}

char *xstrndup(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static void *xcalloc(size_t n, size_t sz) {
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) {
        fprintf(stderr, "workflow: out of memory\n");
        exit(3);
    }
    return p;
}

/* ------------------------------------------------------------------ */
/* constructors                                                        */
/* ------------------------------------------------------------------ */

static JVal *jnew(JType t) {
    JVal *v = xcalloc(1, sizeof(JVal));
    v->t = t;
    return v;
}

JVal *jnull(void) { return jnew(J_NULL); }

JVal *jbool(bool b) {
    JVal *v = jnew(J_BOOL);
    v->b = b;
    return v;
}

JVal *jint(int64_t x) {
    JVal *v = jnew(J_NUM);
    v->is_int = true;
    v->i = x;
    v->f = (long double)x;
    v->raw = NULL;
    return v;
}

JVal *jstr(const char *s) { return jstrn(s, s ? strlen(s) : 0); }

JVal *jstrn(const char *s, size_t n) {
    JVal *v = jnew(J_STR);
    v->s = xstrndup(s ? s : "", n);
    v->slen = n;
    return v;
}

JVal *jarr(void) { return jnew(J_ARR); }
JVal *jobj(void) { return jnew(J_OBJ); }

void jarr_push(JVal *a, JVal *v) {
    if (a->alen == a->acap) {
        a->acap = a->acap ? a->acap * 2 : 4;
        a->items = xrealloc(a->items, a->acap * sizeof(JVal *));
    }
    a->items[a->alen++] = v;
}

void jobj_put(JVal *o, const char *k, size_t klen, JVal *v) {
    for (size_t i = 0; i < o->olen; i++) {
        if (o->klens[i] == klen && memcmp(o->keys[i], k, klen) == 0) {
            o->vals[i] = v;
            return;
        }
    }
    if (o->olen == o->ocap) {
        o->ocap = o->ocap ? o->ocap * 2 : 4;
        o->keys = xrealloc(o->keys, o->ocap * sizeof(char *));
        o->klens = xrealloc(o->klens, o->ocap * sizeof(size_t));
        o->vals = xrealloc(o->vals, o->ocap * sizeof(JVal *));
    }
    o->keys[o->olen] = xstrndup(k, klen);
    o->klens[o->olen] = klen;
    o->vals[o->olen] = v;
    o->olen++;
}

JVal *jobj_getn(const JVal *o, const char *k, size_t klen) {
    if (!o || o->t != J_OBJ) return NULL;
    for (size_t i = 0; i < o->olen; i++) {
        if (o->klens[i] == klen && memcmp(o->keys[i], k, klen) == 0)
            return o->vals[i];
    }
    return NULL;
}

JVal *jobj_get(const JVal *o, const char *k) {
    return jobj_getn(o, k, strlen(k));
}

int jkeycmp(const char *a, size_t al, const char *b, size_t bl) {
    size_t m = al < bl ? al : bl;
    int c = m ? memcmp(a, b, m) : 0;
    if (c) return c;
    if (al < bl) return -1;
    if (al > bl) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* copies / equality / ordering                                        */
/* ------------------------------------------------------------------ */

JVal *jcopy(const JVal *v) {
    if (!v) return NULL;
    switch (v->t) {
    case J_NULL:
        return jnull();
    case J_BOOL:
        return jbool(v->b);
    case J_NUM: {
        JVal *n = jnew(J_NUM);
        n->i = v->i;
        n->is_int = v->is_int;
        n->f = v->f;
        n->raw = v->raw;
        return n;
    }
    case J_STR:
        return jstrn(v->s, v->slen);
    case J_ARR: {
        JVal *a = jarr();
        for (size_t i = 0; i < v->alen; i++) jarr_push(a, jcopy(v->items[i]));
        return a;
    }
    case J_OBJ: {
        JVal *o = jobj();
        for (size_t i = 0; i < v->olen; i++)
            jobj_put(o, v->keys[i], v->klens[i], jcopy(v->vals[i]));
        return o;
    }
    }
    return jnull();
}

static int numcmp(const JVal *a, const JVal *b) {
    if (a->is_int && b->is_int) {
        if (a->i < b->i) return -1;
        if (a->i > b->i) return 1;
        return 0;
    }
    if (a->f < b->f) return -1;
    if (a->f > b->f) return 1;
    return 0;
}

bool jeq(const JVal *a, const JVal *b) {
    if (!a || !b) return a == b;
    if (a->t == J_NUM && b->t == J_NUM) return numcmp(a, b) == 0;
    if (a->t != b->t) return false;
    switch (a->t) {
    case J_NULL:
        return true;
    case J_BOOL:
        return a->b == b->b;
    case J_STR:
        return a->slen == b->slen && (a->slen == 0 || memcmp(a->s, b->s, a->slen) == 0);
    case J_ARR:
        if (a->alen != b->alen) return false;
        for (size_t i = 0; i < a->alen; i++)
            if (!jeq(a->items[i], b->items[i])) return false;
        return true;
    case J_OBJ:
        if (a->olen != b->olen) return false;
        for (size_t i = 0; i < a->olen; i++) {
            JVal *bv = jobj_getn(b, a->keys[i], a->klens[i]);
            if (!bv) return false;
            if (!jeq(a->vals[i], bv)) return false;
        }
        return true;
    default:
        return false;
    }
}

static int type_rank(const JVal *v) {
    switch (v->t) {
    case J_NUM: return 0;
    case J_STR: return 1;
    case J_BOOL: return 2;
    case J_NULL: return 3;
    case J_ARR: return 4;
    case J_OBJ: return 5;
    }
    return 5;
}

static size_t *sorted_key_indices(const JVal *o) {
    size_t *idx = xmalloc((o->olen ? o->olen : 1) * sizeof(size_t));
    for (size_t i = 0; i < o->olen; i++) idx[i] = i;
    /* insertion sort: stable + tiny objects */
    for (size_t i = 1; i < o->olen; i++) {
        size_t cur = idx[i];
        size_t j = i;
        while (j > 0 &&
               jkeycmp(o->keys[idx[j - 1]], o->klens[idx[j - 1]],
                       o->keys[cur], o->klens[cur]) > 0) {
            idx[j] = idx[j - 1];
            j--;
        }
        idx[j] = cur;
    }
    return idx;
}

int jcmp(const JVal *a, const JVal *b) {
    int ra = type_rank(a), rb = type_rank(b);
    if (ra != rb) return ra < rb ? -1 : 1;
    switch (a->t) {
    case J_NUM:
        return numcmp(a, b);
    case J_STR: {
        int c = jkeycmp(a->s, a->slen, b->s, b->slen);
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    case J_BOOL:
        if (a->b == b->b) return 0;
        return a->b ? 1 : -1;
    case J_NULL:
        return 0;
    case J_ARR: {
        size_t m = a->alen < b->alen ? a->alen : b->alen;
        for (size_t i = 0; i < m; i++) {
            int c = jcmp(a->items[i], b->items[i]);
            if (c) return c;
        }
        if (a->alen < b->alen) return -1;
        if (a->alen > b->alen) return 1;
        return 0;
    }
    case J_OBJ: {
        size_t *ai = sorted_key_indices(a);
        size_t *bi = sorted_key_indices(b);
        size_t m = a->olen < b->olen ? a->olen : b->olen;
        int res = 0;
        for (size_t i = 0; i < m; i++) {
            int c = jkeycmp(a->keys[ai[i]], a->klens[ai[i]],
                            b->keys[bi[i]], b->klens[bi[i]]);
            if (c) { res = c < 0 ? -1 : 1; goto done; }
        }
        if (a->olen < b->olen) res = -1;
        else if (a->olen > b->olen) res = 1;
        else {
            for (size_t i = 0; i < m; i++) {
                int c = jcmp(a->vals[ai[i]], b->vals[bi[i]]);
                if (c) { res = c < 0 ? -1 : 1; goto done; }
            }
        }
    done:
        free(ai);
        free(bi);
        return res;
    }
    }
    return 0;
}

const char *jtype_name(const JVal *v) {
    switch (v->t) {
    case J_NULL: return "null";
    case J_BOOL: return "boolean";
    case J_NUM: return "number";
    case J_STR: return "string";
    case J_ARR: return "array";
    case J_OBJ: return "object";
    }
    return "null";
}

bool jtruthy(const JVal *v) {
    switch (v->t) {
    case J_NULL: return false;
    case J_BOOL: return v->b;
    case J_NUM:
        if (v->is_int) return v->i != 0;
        return v->f != 0.0L;
    case J_STR: return v->slen != 0;
    case J_ARR: return v->alen != 0;
    case J_OBJ: return v->olen != 0;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* parser                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *p;
    const char *end;
    char *err;
    size_t errlen;
    bool failed;
    int depth;
} Parser;

static void pset(Parser *p, const char *msg) {
    if (!p->failed) {
        p->failed = true;
        snprintf(p->err, p->errlen, "%s", msg);
    }
}

static void skip_ws(Parser *p) {
    while (p->p < p->end) {
        char c = *p->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->p++;
        else break;
    }
}

static JVal *parse_value(Parser *p);

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_hex4(Parser *p, unsigned *out) {
    if (p->end - p->p < 4) { pset(p, "truncated \\u escape"); return false; }
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexval(p->p[i]);
        if (h < 0) { pset(p, "invalid \\u escape"); return false; }
        v = (v << 4) | (unsigned)h;
    }
    p->p += 4;
    *out = v;
    return true;
}

static void put_utf8(char **buf, size_t *len, size_t *cap, unsigned cp) {
    char tmp[4];
    size_t n;
    if (cp < 0x80) {
        tmp[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        tmp[0] = (char)(0xC0 | (cp >> 6));
        tmp[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        tmp[0] = (char)(0xE0 | (cp >> 12));
        tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        tmp[0] = (char)(0xF0 | (cp >> 18));
        tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    if (*len + n + 1 > *cap) {
        while (*len + n + 1 > *cap) *cap = *cap ? *cap * 2 : 16;
        *buf = xrealloc(*buf, *cap);
    }
    memcpy(*buf + *len, tmp, n);
    *len += n;
}

/* Parses a string starting at '"'. Returns malloc'd bytes + length. */
static char *parse_string_raw(Parser *p, size_t *outlen) {
    if (p->p >= p->end || *p->p != '"') { pset(p, "expected string"); return NULL; }
    p->p++;
    size_t cap = 16, len = 0;
    char *buf = xmalloc(cap);
    for (;;) {
        if (p->p >= p->end) { pset(p, "unterminated string"); return NULL; }
        unsigned char c = (unsigned char)*p->p;
        if (c == '"') { p->p++; break; }
        if (c == '\\') {
            p->p++;
            if (p->p >= p->end) { pset(p, "unterminated escape"); return NULL; }
            char e = *p->p++;
            switch (e) {
            case '"': put_utf8(&buf, &len, &cap, '"'); break;
            case '\\': put_utf8(&buf, &len, &cap, '\\'); break;
            case '/': put_utf8(&buf, &len, &cap, '/'); break;
            case 'b': put_utf8(&buf, &len, &cap, 0x08); break;
            case 'f': put_utf8(&buf, &len, &cap, 0x0c); break;
            case 'n': put_utf8(&buf, &len, &cap, 0x0a); break;
            case 'r': put_utf8(&buf, &len, &cap, 0x0d); break;
            case 't': put_utf8(&buf, &len, &cap, 0x09); break;
            case 'u': {
                unsigned cp;
                if (!parse_hex4(p, &cp)) return NULL;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    /* try to combine with a following low surrogate */
                    if (p->end - p->p >= 6 && p->p[0] == '\\' && p->p[1] == 'u') {
                        const char *save = p->p;
                        p->p += 2;
                        unsigned lo;
                        if (!parse_hex4(p, &lo)) return NULL;
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            p->p = save;
                            cp = 0xFFFD;
                        }
                    } else {
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;
                }
                put_utf8(&buf, &len, &cap, cp);
                break;
            }
            default:
                pset(p, "invalid escape sequence");
                return NULL;
            }
            continue;
        }
        if (c < 0x20) { pset(p, "raw control character in string"); return NULL; }
        /* pass through raw bytes (UTF-8) */
        if (len + 2 > cap) {
            while (len + 2 > cap) cap = cap ? cap * 2 : 16;
            buf = xrealloc(buf, cap);
        }
        buf[len++] = (char)c;
        p->p++;
    }
    buf = xrealloc(buf, len + 1);
    buf[len] = '\0';
    *outlen = len;
    return buf;
}

static JVal *parse_number(Parser *p) {
    const char *start = p->p;
    if (p->p < p->end && *p->p == '-') p->p++;
    if (p->p >= p->end) { pset(p, "invalid number"); return NULL; }
    if (*p->p == '0') {
        p->p++;
    } else if (*p->p >= '1' && *p->p <= '9') {
        while (p->p < p->end && *p->p >= '0' && *p->p <= '9') p->p++;
    } else {
        pset(p, "invalid number");
        return NULL;
    }
    bool isint = true;
    if (p->p < p->end && *p->p == '.') {
        isint = false;
        p->p++;
        if (p->p >= p->end || *p->p < '0' || *p->p > '9') { pset(p, "invalid fraction"); return NULL; }
        while (p->p < p->end && *p->p >= '0' && *p->p <= '9') p->p++;
    }
    if (p->p < p->end && (*p->p == 'e' || *p->p == 'E')) {
        isint = false;
        p->p++;
        if (p->p < p->end && (*p->p == '+' || *p->p == '-')) p->p++;
        if (p->p >= p->end || *p->p < '0' || *p->p > '9') { pset(p, "invalid exponent"); return NULL; }
        while (p->p < p->end && *p->p >= '0' && *p->p <= '9') p->p++;
    }
    char *raw = xstrndup(start, (size_t)(p->p - start));
    JVal *v = jnew(J_NUM);
    v->raw = raw;
    if (isint) {
        errno = 0;
        char *ep = NULL;
        long long x = strtoll(raw, &ep, 10);
        if (errno == 0 && ep && *ep == '\0') {
            v->is_int = true;
            v->i = (int64_t)x;
            v->f = (long double)x;
        } else {
            v->is_int = false;
            v->f = strtold(raw, NULL);
        }
    } else {
        v->is_int = false;
        v->f = strtold(raw, NULL);
    }
    return v;
}

static JVal *parse_array(Parser *p) {
    p->p++; /* '[' */
    JVal *a = jarr();
    skip_ws(p);
    if (p->p < p->end && *p->p == ']') { p->p++; return a; }
    for (;;) {
        JVal *v = parse_value(p);
        if (!v) return NULL;
        jarr_push(a, v);
        skip_ws(p);
        if (p->p >= p->end) { pset(p, "unterminated array"); return NULL; }
        if (*p->p == ',') { p->p++; continue; }
        if (*p->p == ']') { p->p++; break; }
        pset(p, "expected ',' or ']'");
        return NULL;
    }
    return a;
}

static JVal *parse_object(Parser *p) {
    p->p++; /* '{' */
    JVal *o = jobj();
    skip_ws(p);
    if (p->p < p->end && *p->p == '}') { p->p++; return o; }
    for (;;) {
        skip_ws(p);
        size_t klen = 0;
        char *k = parse_string_raw(p, &klen);
        if (!k) return NULL;
        skip_ws(p);
        if (p->p >= p->end || *p->p != ':') {
            pset(p, "expected ':'");
            free(k);
            return NULL;
        }
        p->p++;
        JVal *v = parse_value(p);
        if (!v) { free(k); return NULL; }
        jobj_put(o, k, klen, v);
        free(k);
        skip_ws(p);
        if (p->p >= p->end) { pset(p, "unterminated object"); return NULL; }
        if (*p->p == ',') { p->p++; continue; }
        if (*p->p == '}') { p->p++; break; }
        pset(p, "expected ',' or '}'");
        return NULL;
    }
    return o;
}

static JVal *parse_value(Parser *p) {
    skip_ws(p);
    if (p->p >= p->end) { pset(p, "unexpected end of input"); return NULL; }
    if (++p->depth > 200) { pset(p, "nesting too deep"); return NULL; }
    JVal *v = NULL;
    char c = *p->p;
    if (c == 'n') {
        if (p->end - p->p >= 4 && memcmp(p->p, "null", 4) == 0) {
            p->p += 4;
            v = jnull();
        } else pset(p, "invalid literal");
    } else if (c == 't') {
        if (p->end - p->p >= 4 && memcmp(p->p, "true", 4) == 0) {
            p->p += 4;
            v = jbool(true);
        } else pset(p, "invalid literal");
    } else if (c == 'f') {
        if (p->end - p->p >= 5 && memcmp(p->p, "false", 5) == 0) {
            p->p += 5;
            v = jbool(false);
        } else pset(p, "invalid literal");
    } else if (c == '"') {
        size_t n = 0;
        char *s = parse_string_raw(p, &n);
        if (s) {
            v = jnew(J_STR);
            v->s = s;
            v->slen = n;
        }
    } else if (c == '[') {
        v = parse_array(p);
    } else if (c == '{') {
        v = parse_object(p);
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        v = parse_number(p);
    } else {
        pset(p, "unexpected character");
    }
    p->depth--;
    return v;
}

JVal *jparse(const char *buf, size_t len, char *err, size_t errlen) {
    Parser p;
    p.p = buf;
    p.end = buf + len;
    p.err = err;
    p.errlen = errlen;
    p.failed = false;
    p.depth = 0;
    if (err && errlen) err[0] = '\0';
    JVal *v = parse_value(&p);
    if (!v) return NULL;
    skip_ws(&p);
    if (p.p != p.end) {
        pset(&p, "trailing content after JSON document");
        return NULL;
    }
    return v;
}

/* ------------------------------------------------------------------ */
/* writer                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    char *p;
    size_t len, cap;
} Buf;

static void bputc(Buf *b, char c) {
    if (b->len + 2 > b->cap) {
        b->cap = b->cap ? b->cap * 2 : 64;
        b->p = xrealloc(b->p, b->cap);
    }
    b->p[b->len++] = c;
}

static void bput(Buf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        while (b->len + n + 1 > b->cap) b->cap = b->cap ? b->cap * 2 : 64;
        b->p = xrealloc(b->p, b->cap);
    }
    if (n) memcpy(b->p + b->len, s, n);
    b->len += n;
}

static void bputs(Buf *b, const char *s) { bput(b, s, strlen(s)); }

static void write_string(Buf *b, const char *s, size_t n) {
    static const char hex[] = "0123456789abcdef";
    bputc(b, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': bputs(b, "\\\""); break;
        case '\\': bputs(b, "\\\\"); break;
        case '\b': bputs(b, "\\b"); break;
        case '\f': bputs(b, "\\f"); break;
        case '\n': bputs(b, "\\n"); break;
        case '\r': bputs(b, "\\r"); break;
        case '\t': bputs(b, "\\t"); break;
        default:
            if (c < 0x20) {
                char tmp[6] = { '\\', 'u', '0', '0', hex[c >> 4], hex[c & 15] };
                bput(b, tmp, 6);
            } else {
                bputc(b, (char)c);
            }
        }
    }
    bputc(b, '"');
}

static void wval(Buf *b, const JVal *v) {
    switch (v->t) {
    case J_NULL:
        bputs(b, "null");
        break;
    case J_BOOL:
        bputs(b, v->b ? "true" : "false");
        break;
    case J_NUM:
        if (v->is_int) {
            char tmp[32];
            int n = snprintf(tmp, sizeof(tmp), "%lld", (long long)v->i);
            bput(b, tmp, (size_t)n);
        } else if (v->raw) {
            bputs(b, v->raw);
        } else {
            char tmp[64];
            int n = snprintf(tmp, sizeof(tmp), "%.17Lg", v->f);
            bput(b, tmp, (size_t)n);
        }
        break;
    case J_STR:
        write_string(b, v->s, v->slen);
        break;
    case J_ARR:
        bputc(b, '[');
        for (size_t i = 0; i < v->alen; i++) {
            if (i) bputc(b, ',');
            wval(b, v->items[i]);
        }
        bputc(b, ']');
        break;
    case J_OBJ:
        bputc(b, '{');
        for (size_t i = 0; i < v->olen; i++) {
            if (i) bputc(b, ',');
            write_string(b, v->keys[i], v->klens[i]);
            bputc(b, ':');
            wval(b, v->vals[i]);
        }
        bputc(b, '}');
        break;
    }
}

char *jwrite_alloc(const JVal *v, size_t *len) {
    Buf b = { NULL, 0, 0 };
    wval(&b, v);
    bputc(&b, '\0');
    if (len) *len = b.len - 1;
    return b.p;
}

void jwrite(FILE *out, const JVal *v) {
    size_t n = 0;
    char *s = jwrite_alloc(v, &n);
    if (n) fwrite(s, 1, n, out);
    free(s);
}
