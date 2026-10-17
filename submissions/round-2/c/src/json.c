#include "json.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* arena                                                               */
/* ------------------------------------------------------------------ */

struct ArenaBlock {
    ArenaBlock *next;
    size_t used;
    size_t cap;
    unsigned char data[1];
};

static void oom(void)
{
    /* Best effort: the driver prints the canonical internal-error document. */
    fputs("{\"status\":\"failed\",\"results\":{},\"execution_order\":[],\"skipped\":[],"
          "\"error\":{\"code\":\"INTERNAL_ERROR\",\"step\":null,\"message\":\"out of memory\"}}\n",
          stdout);
    exit(3);
}

void arena_init(Arena *a) { a->head = NULL; }

void *arena_alloc(Arena *a, size_t n)
{
    ArenaBlock *b;
    size_t need;
    size_t cap;

    n = (n + 15u) & ~(size_t)15u;
    if (n == 0)
        n = 16;

    b = a->head;
    if (!b || b->used + n > b->cap) {
        cap = n > 65536u ? n : 65536u;
        /* data[1] already provides one byte of storage */
        b = (ArenaBlock *)malloc(sizeof(ArenaBlock) + cap);
        if (!b)
            oom();
        b->next = a->head;
        b->used = 0;
        b->cap = cap;
        a->head = b;
    }
    need = b->used;
    b->used += n;
    return b->data + need;
}

char *arena_strndup(Arena *a, const char *s, size_t n)
{
    char *p = (char *)arena_alloc(a, n + 1);
    if (n)
        memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/* ------------------------------------------------------------------ */
/* buffer                                                              */
/* ------------------------------------------------------------------ */

void buf_init(Buf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_free(Buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static void buf_reserve(Buf *b, size_t extra)
{
    size_t need = b->len + extra;
    size_t cap;
    char *p;
    if (need <= b->cap)
        return;
    cap = b->cap ? b->cap : 256;
    while (cap < need)
        cap *= 2;
    p = (char *)realloc(b->data, cap);
    if (!p)
        oom();
    b->data = p;
    b->cap = cap;
}

void buf_append(Buf *b, const char *s, size_t n)
{
    buf_reserve(b, n);
    if (n)
        memcpy(b->data + b->len, s, n);
    b->len += n;
}

void buf_putc(Buf *b, char c)
{
    buf_reserve(b, 1);
    b->data[b->len++] = c;
}

/* ------------------------------------------------------------------ */
/* constructors                                                        */
/* ------------------------------------------------------------------ */

JVal *jv_null(Arena *a)
{
    JVal *v = (JVal *)arena_alloc(a, sizeof(JVal));
    memset(v, 0, sizeof(*v));
    v->type = J_NULL;
    return v;
}

JVal *jv_bool(Arena *a, int b)
{
    JVal *v = (JVal *)arena_alloc(a, sizeof(JVal));
    memset(v, 0, sizeof(*v));
    v->type = J_BOOL;
    v->u.boolean = b ? 1 : 0;
    return v;
}

JVal *jv_int(Arena *a, int64_t i)
{
    JVal *v = (JVal *)arena_alloc(a, sizeof(JVal));
    memset(v, 0, sizeof(*v));
    v->type = J_NUM;
    v->u.num.is_int = 1;
    v->u.num.fits = 1;
    v->u.num.i = i;
    v->u.num.d = (double)i;
    v->u.num.raw = NULL;
    return v;
}

JVal *jv_str(Arena *a, const char *s, size_t n)
{
    JVal *v = (JVal *)arena_alloc(a, sizeof(JVal));
    char *copy = (char *)arena_alloc(a, n + 1);
    memset(v, 0, sizeof(*v));
    if (n)
        memcpy(copy, s, n);
    copy[n] = '\0';
    v->type = J_STR;
    v->u.str.s = copy;
    v->u.str.len = n;
    return v;
}

JVal *jv_arr(Arena *a)
{
    JVal *v = (JVal *)arena_alloc(a, sizeof(JVal));
    memset(v, 0, sizeof(*v));
    v->type = J_ARR;
    return v;
}

void jv_arr_push(Arena *a, JVal *arr, JVal *item)
{
    if (arr->u.arr.len == arr->u.arr.cap) {
        size_t ncap = arr->u.arr.cap ? arr->u.arr.cap * 2 : 8;
        JVal **items = (JVal **)arena_alloc(a, ncap * sizeof(JVal *));
        if (arr->u.arr.len)
            memcpy(items, arr->u.arr.items, arr->u.arr.len * sizeof(JVal *));
        arr->u.arr.items = items;
        arr->u.arr.cap = ncap;
    }
    arr->u.arr.items[arr->u.arr.len++] = item;
}

JVal *jv_obj(Arena *a)
{
    JVal *v = (JVal *)arena_alloc(a, sizeof(JVal));
    memset(v, 0, sizeof(*v));
    v->type = J_OBJ;
    return v;
}

static void obj_grow(Arena *a, JVal *obj)
{
    if (obj->u.obj.len == obj->u.obj.cap) {
        size_t ncap = obj->u.obj.cap ? obj->u.obj.cap * 2 : 8;
        JPair *pairs = (JPair *)arena_alloc(a, ncap * sizeof(JPair));
        if (obj->u.obj.len)
            memcpy(pairs, obj->u.obj.pairs, obj->u.obj.len * sizeof(JPair));
        obj->u.obj.pairs = pairs;
        obj->u.obj.cap = ncap;
    }
}

void jv_obj_set(Arena *a, JVal *obj, const char *key, size_t klen, JVal *val)
{
    size_t i;
    for (i = 0; i < obj->u.obj.len; i++) {
        JPair *p = &obj->u.obj.pairs[i];
        if (p->klen == klen && (klen == 0 || memcmp(p->key, key, klen) == 0)) {
            p->val = val;
            return;
        }
    }
    jv_obj_append(a, obj, key, klen, val);
}

void jv_obj_append(Arena *a, JVal *obj, const char *key, size_t klen, JVal *val)
{
    JPair *p;
    obj_grow(a, obj);
    p = &obj->u.obj.pairs[obj->u.obj.len++];
    p->key = arena_strndup(a, key, klen);
    p->klen = klen;
    p->val = val;
}

JVal *jv_obj_get_n(const JVal *obj, const char *key, size_t klen)
{
    size_t i;
    if (!obj || obj->type != J_OBJ)
        return NULL;
    for (i = obj->u.obj.len; i > 0; i--) {
        JPair *p = &obj->u.obj.pairs[i - 1];
        if (p->klen == klen && (klen == 0 || memcmp(p->key, key, klen) == 0))
            return p->val;
    }
    return NULL;
}

JVal *jv_obj_get_first_n(const JVal *obj, const char *key, size_t klen)
{
    size_t i;
    if (!obj || obj->type != J_OBJ)
        return NULL;
    for (i = 0; i < obj->u.obj.len; i++) {
        JPair *p = &obj->u.obj.pairs[i];
        if (p->klen == klen && (klen == 0 || memcmp(p->key, key, klen) == 0))
            return p->val;
    }
    return NULL;
}

JVal *jv_obj_get_last(const JVal *obj, const char *key)
{
    return jv_obj_get_n(obj, key, strlen(key));
}

/* ------------------------------------------------------------------ */
/* comparison / equality                                               */
/* ------------------------------------------------------------------ */

int cmp_bytes(const char *a, size_t alen, const char *b, size_t blen)
{
    size_t n = alen < blen ? alen : blen;
    int c = n ? memcmp(a, b, n) : 0;
    if (c)
        return c < 0 ? -1 : 1;
    if (alen == blen)
        return 0;
    return alen < blen ? -1 : 1;
}

static int num_is_exact_int(const JVal *v)
{
    return v->u.num.is_int && v->u.num.fits;
}

/* Produce the sign and decimal digits of an integer-literal number. */
static void int_decimal(const JVal *v, char *buf, size_t bufsz, int *neg,
                        const char **digits, size_t *nd)
{
    const char *r;
    size_t n, i = 0;
    if (v->u.num.raw) {
        r = v->u.num.raw;
        n = strlen(r);
    } else {
        snprintf(buf, bufsz, "%lld", (long long)v->u.num.i);
        r = buf;
        n = strlen(r);
    }
    *neg = 0;
    if (n && r[0] == '-') {
        *neg = 1;
        i = 1;
    }
    while (i + 1 < n && r[i] == '0')
        i++;
    *digits = r + i;
    *nd = n - i;
    if (*nd == 1 && (*digits)[0] == '0')
        *neg = 0;
}

static int cmp_int_exact(const JVal *a, const JVal *b)
{
    char ba[32], bb[32];
    int an, bn;
    const char *ad, *bd;
    size_t al, bl;
    int mag;
    int_decimal(a, ba, sizeof(ba), &an, &ad, &al);
    int_decimal(b, bb, sizeof(bb), &bn, &bd, &bl);
    if (an != bn)
        return an ? -1 : 1;
    if (al != bl)
        mag = al < bl ? -1 : 1;
    else {
        int c = al ? memcmp(ad, bd, al) : 0;
        mag = c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    return an ? -mag : mag;
}

static int num_cmp(const JVal *a, const JVal *b)
{
    if (num_is_exact_int(a) && num_is_exact_int(b)) {
        if (a->u.num.i < b->u.num.i)
            return -1;
        if (a->u.num.i > b->u.num.i)
            return 1;
        return 0;
    }
    if (a->u.num.is_int && b->u.num.is_int)
        return cmp_int_exact(a, b);
    {
        double x = (a->u.num.is_int && a->u.num.fits) ? (double)a->u.num.i : a->u.num.d;
        double y = (b->u.num.is_int && b->u.num.fits) ? (double)b->u.num.i : b->u.num.d;
        if (x < y)
            return -1;
        if (x > y)
            return 1;
        return 0;
    }
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

static void sort_indices(const JVal *obj, size_t *idx)
{
    size_t n = obj->u.obj.len;
    size_t i;
    for (i = 0; i < n; i++)
        idx[i] = i;
    /* insertion sort; objects are normally tiny */
    for (i = 1; i < n; i++) {
        size_t key = idx[i];
        size_t j = i;
        while (j > 0) {
            JPair *pa = &obj->u.obj.pairs[idx[j - 1]];
            JPair *pb = &obj->u.obj.pairs[key];
            if (cmp_bytes(pa->key, pa->klen, pb->key, pb->klen) <= 0)
                break;
            idx[j] = idx[j - 1];
            j--;
        }
        idx[j] = key;
    }
}

static int cmp_obj(const JVal *a, const JVal *b)
{
    size_t na = a->u.obj.len, nb = b->u.obj.len;
    size_t *ia, *ib;
    size_t i, n;
    int c = 0;

    ia = (size_t *)malloc((na ? na : 1) * sizeof(size_t));
    ib = (size_t *)malloc((nb ? nb : 1) * sizeof(size_t));
    if (!ia || !ib)
        oom();
    sort_indices(a, ia);
    sort_indices(b, ib);

    n = na < nb ? na : nb;
    for (i = 0; i < n && c == 0; i++) {
        JPair *pa = &a->u.obj.pairs[ia[i]];
        JPair *pb = &b->u.obj.pairs[ib[i]];
        c = cmp_bytes(pa->key, pa->klen, pb->key, pb->klen);
    }
    if (c == 0 && na != nb)
        c = na < nb ? -1 : 1;
    for (i = 0; i < n && c == 0; i++) {
        c = jv_cmp(a->u.obj.pairs[ia[i]].val, b->u.obj.pairs[ib[i]].val);
    }
    free(ia);
    free(ib);
    return c;
}

int jv_cmp(const JVal *a, const JVal *b)
{
    int ra = type_rank(a), rb = type_rank(b);
    size_t i, n;
    if (ra != rb)
        return ra < rb ? -1 : 1;
    switch (a->type) {
    case J_NULL:
        return 0;
    case J_BOOL:
        if (a->u.boolean == b->u.boolean)
            return 0;
        return a->u.boolean < b->u.boolean ? -1 : 1;
    case J_NUM:
        return num_cmp(a, b);
    case J_STR:
        return cmp_bytes(a->u.str.s, a->u.str.len, b->u.str.s, b->u.str.len);
    case J_ARR:
        n = a->u.arr.len < b->u.arr.len ? a->u.arr.len : b->u.arr.len;
        for (i = 0; i < n; i++) {
            int c = jv_cmp(a->u.arr.items[i], b->u.arr.items[i]);
            if (c)
                return c;
        }
        if (a->u.arr.len == b->u.arr.len)
            return 0;
        return a->u.arr.len < b->u.arr.len ? -1 : 1;
    case J_OBJ:
        return cmp_obj(a, b);
    }
    return 0;
}

static int obj_equal(const JVal *a, const JVal *b)
{
    size_t i, j;
    unsigned char *used;
    if (a->u.obj.len != b->u.obj.len)
        return 0;
    if (a->u.obj.len == 0)
        return 1;
    used = (unsigned char *)calloc(b->u.obj.len, 1);
    if (!used)
        oom();
    for (i = 0; i < a->u.obj.len; i++) {
        JPair *pa = &a->u.obj.pairs[i];
        int found = 0;
        for (j = 0; j < b->u.obj.len; j++) {
            JPair *pb = &b->u.obj.pairs[j];
            if (!used[j] && pa->klen == pb->klen &&
                (pa->klen == 0 || memcmp(pa->key, pb->key, pa->klen) == 0) &&
                jv_equal(pa->val, pb->val)) {
                used[j] = 1;
                found = 1;
                break;
            }
        }
        if (!found) {
            free(used);
            return 0;
        }
    }
    free(used);
    return 1;
}

int jv_equal(const JVal *a, const JVal *b)
{
    size_t i;
    if (a->type != b->type)
        return 0;
    switch (a->type) {
    case J_NULL: return 1;
    case J_BOOL: return a->u.boolean == b->u.boolean;
    case J_NUM: return num_cmp(a, b) == 0;
    case J_STR:
        return a->u.str.len == b->u.str.len &&
               (a->u.str.len == 0 ||
                memcmp(a->u.str.s, b->u.str.s, a->u.str.len) == 0);
    case J_ARR:
        if (a->u.arr.len != b->u.arr.len)
            return 0;
        for (i = 0; i < a->u.arr.len; i++)
            if (!jv_equal(a->u.arr.items[i], b->u.arr.items[i]))
                return 0;
        return 1;
    case J_OBJ:
        return obj_equal(a, b);
    }
    return 0;
}

int jv_truthy(const JVal *v)
{
    switch (v->type) {
    case J_NULL: return 0;
    case J_BOOL: return v->u.boolean != 0;
    case J_NUM:
        if (num_is_exact_int(v))
            return v->u.num.i != 0;
        return v->u.num.d != 0.0;
    case J_STR: return v->u.str.len != 0;
    case J_ARR: return v->u.arr.len != 0;
    case J_OBJ: return v->u.obj.len != 0;
    }
    return 0;
}

const char *jv_type_name(const JVal *v)
{
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

/* ------------------------------------------------------------------ */
/* parser                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    Arena *a;
    const char *s;
    size_t len;
    size_t pos;
    const char *err;
} Parser;

static JVal *parse_value(Parser *p, int depth);

static JVal *perr(Parser *p, const char *msg)
{
    if (!p->err)
        p->err = msg;
    return NULL;
}

static void skip_ws(Parser *p)
{
    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            p->pos++;
        else
            break;
    }
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_hex4(Parser *p, unsigned *out)
{
    unsigned v = 0;
    int i;
    if (p->pos + 4 > p->len)
        return 0;
    for (i = 0; i < 4; i++) {
        int h = hexval((unsigned char)p->s[p->pos + i]);
        if (h < 0)
            return 0;
        v = (v << 4) | (unsigned)h;
    }
    p->pos += 4;
    *out = v;
    return 1;
}

static void put_utf8(Buf *b, unsigned cp)
{
    char tmp[4];
    if (cp < 0x80) {
        tmp[0] = (char)cp;
        buf_append(b, tmp, 1);
    } else if (cp < 0x800) {
        tmp[0] = (char)(0xC0 | (cp >> 6));
        tmp[1] = (char)(0x80 | (cp & 0x3F));
        buf_append(b, tmp, 2);
    } else if (cp < 0x10000) {
        tmp[0] = (char)(0xE0 | (cp >> 12));
        tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[2] = (char)(0x80 | (cp & 0x3F));
        buf_append(b, tmp, 3);
    } else {
        tmp[0] = (char)(0xF0 | (cp >> 18));
        tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[3] = (char)(0x80 | (cp & 0x3F));
        buf_append(b, tmp, 4);
    }
}

static JVal *parse_string(Parser *p, Arena *a)
{
    Buf b;
    if (p->pos >= p->len || p->s[p->pos] != '"')
        return perr(p, "expected string");
    p->pos++;
    buf_init(&b);
    while (1) {
        unsigned char c;
        if (p->pos >= p->len) {
            buf_free(&b);
            return perr(p, "unterminated string");
        }
        c = (unsigned char)p->s[p->pos++];
        if (c == '"')
            break;
        if (c < 0x20) {
            buf_free(&b);
            return perr(p, "control character in string");
        }
        if (c == '\\') {
            if (p->pos >= p->len) {
                buf_free(&b);
                return perr(p, "unterminated escape");
            }
            c = (unsigned char)p->s[p->pos++];
            switch (c) {
            case '"': buf_putc(&b, '"'); break;
            case '\\': buf_putc(&b, '\\'); break;
            case '/': buf_putc(&b, '/'); break;
            case 'b': buf_putc(&b, '\b'); break;
            case 'f': buf_putc(&b, '\f'); break;
            case 'n': buf_putc(&b, '\n'); break;
            case 'r': buf_putc(&b, '\r'); break;
            case 't': buf_putc(&b, '\t'); break;
            case 'u': {
                unsigned cp;
                if (!parse_hex4(p, &cp)) {
                    buf_free(&b);
                    return perr(p, "bad unicode escape");
                }
                if (cp >= 0xD800 && cp <= 0xDBFF && p->pos + 6 <= p->len &&
                    p->s[p->pos] == '\\' && p->s[p->pos + 1] == 'u') {
                    size_t save = p->pos;
                    unsigned lo;
                    p->pos += 2;
                    if (parse_hex4(p, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else {
                        p->pos = save;
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                    cp = 0xFFFD;
                }
                put_utf8(&b, cp);
                break;
            }
            default:
                buf_free(&b);
                return perr(p, "bad escape");
            }
        } else {
            buf_putc(&b, (char)c);
        }
    }
    {
        JVal *v = jv_str(a, b.data ? b.data : "", b.len);
        buf_free(&b);
        return v;
    }
}

static JVal *parse_number(Parser *p, Arena *a)
{
    size_t start = p->pos;
    int is_int = 1;
    char *raw;
    JVal *v;
    char *endp = NULL;
    long long ll;
    double d;

    if (p->pos < p->len && p->s[p->pos] == '-')
        p->pos++;
    if (p->pos >= p->len)
        return perr(p, "bad number");
    if (p->s[p->pos] == '0') {
        p->pos++;
    } else if (p->s[p->pos] >= '1' && p->s[p->pos] <= '9') {
        while (p->pos < p->len && p->s[p->pos] >= '0' && p->s[p->pos] <= '9')
            p->pos++;
    } else {
        return perr(p, "bad number");
    }
    if (p->pos < p->len && p->s[p->pos] == '.') {
        is_int = 0;
        p->pos++;
        if (p->pos >= p->len || p->s[p->pos] < '0' || p->s[p->pos] > '9')
            return perr(p, "bad number fraction");
        while (p->pos < p->len && p->s[p->pos] >= '0' && p->s[p->pos] <= '9')
            p->pos++;
    }
    if (p->pos < p->len && (p->s[p->pos] == 'e' || p->s[p->pos] == 'E')) {
        is_int = 0;
        p->pos++;
        if (p->pos < p->len && (p->s[p->pos] == '+' || p->s[p->pos] == '-'))
            p->pos++;
        if (p->pos >= p->len || p->s[p->pos] < '0' || p->s[p->pos] > '9')
            return perr(p, "bad number exponent");
        while (p->pos < p->len && p->s[p->pos] >= '0' && p->s[p->pos] <= '9')
            p->pos++;
    }

    raw = arena_strndup(a, p->s + start, p->pos - start);
    v = (JVal *)arena_alloc(a, sizeof(JVal));
    memset(v, 0, sizeof(*v));
    v->type = J_NUM;
    v->u.num.raw = raw;
    v->u.num.is_int = is_int;

    errno = 0;
    ll = strtoll(raw, &endp, 10);
    if (is_int && errno != ERANGE && endp && *endp == '\0') {
        v->u.num.fits = 1;
        v->u.num.i = (int64_t)ll;
    } else {
        v->u.num.fits = 0;
        v->u.num.i = 0;
    }
    d = strtod(raw, &endp);
    v->u.num.d = d;
    return v;
}

static JVal *parse_array(Parser *p, int depth)
{
    JVal *arr;
    if (p->pos >= p->len || p->s[p->pos] != '[')
        return perr(p, "expected array");
    p->pos++;
    arr = jv_arr(p->a);
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == ']') {
        p->pos++;
        return arr;
    }
    while (1) {
        JVal *item;
        skip_ws(p);
        item = parse_value(p, depth + 1);
        if (!item)
            return NULL;
        jv_arr_push(p->a, arr, item);
        skip_ws(p);
        if (p->pos >= p->len)
            return perr(p, "unterminated array");
        if (p->s[p->pos] == ',') {
            p->pos++;
            continue;
        }
        if (p->s[p->pos] == ']') {
            p->pos++;
            return arr;
        }
        return perr(p, "expected ',' or ']'");
    }
}

static JVal *parse_object(Parser *p, int depth)
{
    JVal *obj;
    if (p->pos >= p->len || p->s[p->pos] != '{')
        return perr(p, "expected object");
    p->pos++;
    obj = jv_obj(p->a);
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == '}') {
        p->pos++;
        return obj;
    }
    while (1) {
        JVal *keyval;
        JVal *val;
        skip_ws(p);
        keyval = parse_string(p, p->a);
        if (!keyval)
            return NULL;
        skip_ws(p);
        if (p->pos >= p->len || p->s[p->pos] != ':')
            return perr(p, "expected ':'");
        p->pos++;
        skip_ws(p);
        val = parse_value(p, depth + 1);
        if (!val)
            return NULL;
        jv_obj_append(p->a, obj, keyval->u.str.s, keyval->u.str.len, val);
        skip_ws(p);
        if (p->pos >= p->len)
            return perr(p, "unterminated object");
        if (p->s[p->pos] == ',') {
            p->pos++;
            continue;
        }
        if (p->s[p->pos] == '}') {
            p->pos++;
            return obj;
        }
        return perr(p, "expected ',' or '}'");
    }
}

static JVal *parse_value(Parser *p, int depth)
{
    if (depth > 1000)
        return perr(p, "nesting too deep");
    skip_ws(p);
    if (p->pos >= p->len)
        return perr(p, "unexpected end of input");
    switch (p->s[p->pos]) {
    case '{': return parse_object(p, depth);
    case '[': return parse_array(p, depth);
    case '"': return parse_string(p, p->a);
    case 't':
        if (p->len - p->pos >= 4 && memcmp(p->s + p->pos, "true", 4) == 0) {
            p->pos += 4;
            return jv_bool(p->a, 1);
        }
        return perr(p, "bad literal");
    case 'f':
        if (p->len - p->pos >= 5 && memcmp(p->s + p->pos, "false", 5) == 0) {
            p->pos += 5;
            return jv_bool(p->a, 0);
        }
        return perr(p, "bad literal");
    case 'n':
        if (p->len - p->pos >= 4 && memcmp(p->s + p->pos, "null", 4) == 0) {
            p->pos += 4;
            return jv_null(p->a);
        }
        return perr(p, "bad literal");
    default:
        if (p->s[p->pos] == '-' || (p->s[p->pos] >= '0' && p->s[p->pos] <= '9'))
            return parse_number(p, p->a);
        return perr(p, "unexpected character");
    }
}

JVal *json_parse(Arena *a, const char *src, size_t len, const char **errmsg)
{
    Parser p;
    JVal *v;
    p.a = a;
    p.s = src;
    p.len = len;
    p.pos = 0;
    p.err = NULL;
    /* tolerate a UTF-8 BOM */
    if (len >= 3 && (unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBB &&
        (unsigned char)src[2] == 0xBF)
        p.pos = 3;
    v = parse_value(&p, 0);
    if (!v) {
        if (errmsg)
            *errmsg = p.err ? p.err : "invalid JSON";
        return NULL;
    }
    skip_ws(&p);
    if (p.pos != p.len) {
        if (errmsg)
            *errmsg = "trailing data after JSON document";
        return NULL;
    }
    return v;
}

/* ------------------------------------------------------------------ */
/* emitter                                                             */
/* ------------------------------------------------------------------ */

void json_write_string(Buf *b, const char *s, size_t n)
{
    size_t i;
    static const char hex[] = "0123456789abcdef";
    buf_putc(b, '"');
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': buf_append(b, "\\\"", 2); break;
        case '\\': buf_append(b, "\\\\", 2); break;
        case '\b': buf_append(b, "\\b", 2); break;
        case '\f': buf_append(b, "\\f", 2); break;
        case '\n': buf_append(b, "\\n", 2); break;
        case '\r': buf_append(b, "\\r", 2); break;
        case '\t': buf_append(b, "\\t", 2); break;
        default:
            if (c < 0x20) {
                char tmp[6];
                tmp[0] = '\\';
                tmp[1] = 'u';
                tmp[2] = '0';
                tmp[3] = '0';
                tmp[4] = hex[(c >> 4) & 0xF];
                tmp[5] = hex[c & 0xF];
                buf_append(b, tmp, 6);
            } else {
                buf_putc(b, (char)c);
            }
        }
    }
    buf_putc(b, '"');
}

static void write_int(Buf *b, int64_t v)
{
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
    if (n > 0)
        buf_append(b, tmp, (size_t)n);
}

static void write_num(Buf *b, const JVal *v)
{
    if (v->u.num.raw) {
        buf_append(b, v->u.num.raw, strlen(v->u.num.raw));
    } else if (v->u.num.fits) {
        write_int(b, v->u.num.i);
    } else {
        char tmp[64];
        int n = snprintf(tmp, sizeof(tmp), "%.17g", v->u.num.d);
        if (n > 0)
            buf_append(b, tmp, (size_t)n);
    }
}

void json_write(Buf *b, const JVal *v)
{
    size_t i;
    switch (v->type) {
    case J_NULL:
        buf_append(b, "null", 4);
        break;
    case J_BOOL:
        if (v->u.boolean)
            buf_append(b, "true", 4);
        else
            buf_append(b, "false", 5);
        break;
    case J_NUM:
        write_num(b, v);
        break;
    case J_STR:
        json_write_string(b, v->u.str.s, v->u.str.len);
        break;
    case J_ARR:
        buf_putc(b, '[');
        for (i = 0; i < v->u.arr.len; i++) {
            if (i)
                buf_putc(b, ',');
            json_write(b, v->u.arr.items[i]);
        }
        buf_putc(b, ']');
        break;
    case J_OBJ:
        buf_putc(b, '{');
        for (i = 0; i < v->u.obj.len; i++) {
            JPair *p = &v->u.obj.pairs[i];
            if (i)
                buf_putc(b, ',');
            json_write_string(b, p->key, p->klen);
            buf_putc(b, ':');
            json_write(b, p->val);
        }
        buf_putc(b, '}');
        break;
    }
}
