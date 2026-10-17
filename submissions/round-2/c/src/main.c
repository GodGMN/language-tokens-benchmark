#include "json.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================== */
/* Engine data structures                                              */
/* ================================================================== */

typedef struct {
    const char *code;
    char *step; /* may be NULL */
    char msg[256];
} WErr;

typedef struct {
    char *id;
    size_t idlen;
    JVal *op;
    JVal *args;
    JVal *when;
    int *deps;
    int ndeps;
    int max_attempts;
    int done;
    int skipped;
    JVal *result;
    int attempts;
} Step;

typedef struct {
    Arena *a;
    Step *steps;
    int n;
    JVal *results;
    JVal *exec_order;
    JVal *skipped_arr;
    WErr err;
    int has_err;
} Engine;

typedef struct {
    const char *code;
    char msg[256];
} OpErr;

static int oerr(OpErr *e, const char *code, const char *fmt, ...)
{
    va_list ap;
    e->code = code;
    va_start(ap, fmt);
    vsnprintf(e->msg, sizeof(e->msg), fmt, ap);
    va_end(ap);
    return 0;
}

static void set_err(Engine *e, const char *code, const char *step, const char *msg)
{
    if (e->has_err)
        return;
    e->err.code = code;
    e->err.step = step ? arena_strndup(e->a, step, strlen(step)) : NULL;
    snprintf(e->err.msg, sizeof(e->err.msg), "%s", msg ? msg : "");
    e->has_err = 1;
}

static int jstr_eq(const JVal *v, const char *lit)
{
    size_t n = strlen(lit);
    return v && v->type == J_STR && v->u.str.len == n &&
           (n == 0 || memcmp(v->u.str.s, lit, n) == 0);
}

static int pair_key_is(const JPair *p, const char *lit)
{
    size_t n = strlen(lit);
    return p->klen == n && (n == 0 || memcmp(p->key, lit, n) == 0);
}

static int is_valid_type_name(const char *s, size_t n)
{
    static const char *const names[] = {"null", "boolean", "number",
                                        "string", "array", "object"};
    size_t i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strlen(names[i]) == n && memcmp(names[i], s, n) == 0)
            return 1;
    }
    return 0;
}

/* ================================================================== */
/* Small JSON helpers                                                  */
/* ================================================================== */

static int int_of(const JVal *v, int64_t *out)
{
    if (v && v->type == J_NUM && v->u.num.is_int && v->u.num.fits) {
        *out = v->u.num.i;
        return 1;
    }
    return 0;
}

static JVal *str_from_buf(Engine *e, Buf *b)
{
    JVal *v = jv_str(e->a, b->data ? b->data : "", b->len);
    buf_free(b);
    return v;
}

static JVal *obj_copy(Engine *e, JVal *src)
{
    JVal *o = jv_obj(e->a);
    size_t i;
    for (i = 0; i < src->u.obj.len; i++)
        jv_obj_append(e->a, o, src->u.obj.pairs[i].key, src->u.obj.pairs[i].klen,
                      src->u.obj.pairs[i].val);
    return o;
}

static int contains_sub(const char *hay, size_t hn, const char *needle, size_t nn)
{
    size_t i;
    if (nn == 0)
        return 1;
    if (nn > hn)
        return 0;
    for (i = 0; i + nn <= hn; i++) {
        if (memcmp(hay + i, needle, nn) == 0)
            return 1;
    }
    return 0;
}

static size_t utf8_char_len(const char *s, size_t remaining)
{
    unsigned char c = (unsigned char)s[0];
    size_t n;
    if (c < 0x80)
        n = 1;
    else if ((c & 0xE0) == 0xC0)
        n = 2;
    else if ((c & 0xF0) == 0xE0)
        n = 3;
    else if ((c & 0xF8) == 0xF0)
        n = 4;
    else
        n = 1;
    if (n > remaining)
        n = remaining;
    return n;
}

/* ================================================================== */
/* Argument access                                                     */
/* ================================================================== */

static int allow_keys(JVal *args, const char *const *keys, size_t nk, OpErr *e)
{
    size_t i, j;
    for (i = 0; i < args->u.obj.len; i++) {
        JPair *p = &args->u.obj.pairs[i];
        int ok = 0;
        for (j = 0; j < nk; j++) {
            if (pair_key_is(p, keys[j])) {
                ok = 1;
                break;
            }
        }
        if (!ok)
            return oerr(e, "INVALID_ARG", "unknown argument '%.*s'", (int)p->klen,
                        p->key);
    }
    return 1;
}

static int get_any(JVal *args, const char *name, JVal **out, OpErr *e)
{
    JVal *v = jv_obj_get_last(args, name);
    if (!v)
        return oerr(e, "INVALID_ARG", "missing argument '%s'", name);
    *out = v;
    return 1;
}

static int get_int(JVal *args, const char *name, int64_t *out, OpErr *e)
{
    JVal *v = jv_obj_get_last(args, name);
    if (!v)
        return oerr(e, "INVALID_ARG", "missing argument '%s'", name);
    if (!int_of(v, out))
        return oerr(e, "INVALID_ARG", "argument '%s' must be an i64 integer", name);
    return 1;
}

static int get_int_opt(JVal *args, const char *name, int *has, int64_t *out, OpErr *e)
{
    JVal *v = jv_obj_get_last(args, name);
    if (!v) {
        *has = 0;
        return 1;
    }
    if (!int_of(v, out))
        return oerr(e, "INVALID_ARG", "argument '%s' must be an i64 integer", name);
    *has = 1;
    return 1;
}

static int get_str(JVal *args, const char *name, const char **s, size_t *n, OpErr *e)
{
    JVal *v = jv_obj_get_last(args, name);
    if (!v)
        return oerr(e, "INVALID_ARG", "missing argument '%s'", name);
    if (v->type != J_STR)
        return oerr(e, "INVALID_ARG", "argument '%s' must be a string", name);
    *s = v->u.str.s;
    *n = v->u.str.len;
    return 1;
}

static int get_arr(JVal *args, const char *name, JVal **out, OpErr *e)
{
    JVal *v = jv_obj_get_last(args, name);
    if (!v)
        return oerr(e, "INVALID_ARG", "missing argument '%s'", name);
    if (v->type != J_ARR)
        return oerr(e, "INVALID_ARG", "argument '%s' must be an array", name);
    *out = v;
    return 1;
}

static int get_obj(JVal *args, const char *name, JVal **out, OpErr *e)
{
    JVal *v = jv_obj_get_last(args, name);
    if (!v)
        return oerr(e, "INVALID_ARG", "missing argument '%s'", name);
    if (v->type != J_OBJ)
        return oerr(e, "INVALID_ARG", "argument '%s' must be an object", name);
    *out = v;
    return 1;
}

/* ================================================================== */
/* Arithmetic                                                          */
/* ================================================================== */

static int math2(const char *op, int64_t a, int64_t b, int64_t *out, OpErr *e)
{
    int64_t r;
    if (strcmp(op, "add") == 0) {
        if (__builtin_add_overflow(a, b, &r))
            return oerr(e, "ARITHMETIC_OVERFLOW", "addition overflow");
        *out = r;
        return 1;
    }
    if (strcmp(op, "subtract") == 0) {
        if (__builtin_sub_overflow(a, b, &r))
            return oerr(e, "ARITHMETIC_OVERFLOW", "subtraction overflow");
        *out = r;
        return 1;
    }
    if (strcmp(op, "multiply") == 0) {
        if (__builtin_mul_overflow(a, b, &r))
            return oerr(e, "ARITHMETIC_OVERFLOW", "multiplication overflow");
        *out = r;
        return 1;
    }
    if (strcmp(op, "divide") == 0) {
        if (b == 0)
            return oerr(e, "DIVISION_BY_ZERO", "division by zero");
        if (a == INT64_MIN && b == -1)
            return oerr(e, "ARITHMETIC_OVERFLOW", "division overflow");
        *out = a / b;
        return 1;
    }
    if (strcmp(op, "mod") == 0) {
        if (b == 0)
            return oerr(e, "DIVISION_BY_ZERO", "modulo by zero");
        if (a == INT64_MIN && b == -1)
            return oerr(e, "ARITHMETIC_OVERFLOW", "modulo overflow");
        *out = a % b;
        return 1;
    }
    return oerr(e, "INVALID_ARG", "unknown math operation '%s'", op);
}

static int is_math2_op(const char *op)
{
    return strcmp(op, "add") == 0 || strcmp(op, "subtract") == 0 ||
           strcmp(op, "multiply") == 0 || strcmp(op, "divide") == 0 ||
           strcmp(op, "mod") == 0;
}

/* ================================================================== */
/* transform / predicate evaluation                                    */
/* ================================================================== */

static JVal *subst_self(Engine *e, JVal *v, JVal *elem)
{
    size_t i;
    if (v->type == J_OBJ && v->u.obj.len == 1 && pair_key_is(&v->u.obj.pairs[0], "$self") &&
        v->u.obj.pairs[0].val->type == J_BOOL && v->u.obj.pairs[0].val->u.boolean)
        return elem;
    if (v->type == J_ARR) {
        JVal *r = jv_arr(e->a);
        for (i = 0; i < v->u.arr.len; i++)
            jv_arr_push(e->a, r, subst_self(e, v->u.arr.items[i], elem));
        return r;
    }
    if (v->type == J_OBJ) {
        JVal *r = jv_obj(e->a);
        for (i = 0; i < v->u.obj.len; i++)
            jv_obj_append(e->a, r, v->u.obj.pairs[i].key, v->u.obj.pairs[i].klen,
                          subst_self(e, v->u.obj.pairs[i].val, elem));
        return r;
    }
    return v;
}

static int apply_transform(Engine *e, JVal *elem, JVal *t, JVal **out, OpErr *err)
{
    static const char *const k_const[] = {"type", "value"};
    static const char *const k_num[] = {"type", "value"};
    static const char *const k_plain[] = {"type"};
    static const char *const k_op[] = {"type", "op", "args"};
    static const char *const k_ab[] = {"a", "b"};
    JVal *tv;
    int64_t iv;

    *out = NULL;
    if (!t || t->type != J_OBJ)
        return oerr(err, "INVALID_ARG", "map transform must be an object");
    tv = jv_obj_get_last(t, "type");
    if (!tv || tv->type != J_STR)
        return oerr(err, "INVALID_ARG", "map transform requires string 'type'");

    if (jstr_eq(tv, "constant")) {
        JVal *v = NULL;
        if (!allow_keys(t, k_const, 2, err))
            return 0;
        if (!get_any(t, "value", &v, err))
            return 0;
        *out = v;
        return 1;
    }
    if (jstr_eq(tv, "add") || jstr_eq(tv, "subtract") || jstr_eq(tv, "multiply") ||
        jstr_eq(tv, "divide") || jstr_eq(tv, "mod")) {
        int64_t ev, r;
        if (!allow_keys(t, k_num, 2, err))
            return 0;
        if (!int_of(elem, &ev))
            return oerr(err, "INVALID_ARG", "map transform requires an integer element");
        if (!get_int(t, "value", &iv, err))
            return 0;
        if (!math2(tv->u.str.s, ev, iv, &r, err))
            return 0;
        *out = jv_int(e->a, r);
        return 1;
    }
    if (jstr_eq(tv, "negate") || jstr_eq(tv, "abs")) {
        int64_t ev;
        if (!allow_keys(t, k_plain, 1, err))
            return 0;
        if (!int_of(elem, &ev))
            return oerr(err, "INVALID_ARG", "map transform requires an integer element");
        if (ev == INT64_MIN)
            return oerr(err, "ARITHMETIC_OVERFLOW", "integer negate/abs overflow");
        *out = jv_int(e->a, jstr_eq(tv, "negate") ? -ev : (ev < 0 ? -ev : ev));
        return 1;
    }
    if (jstr_eq(tv, "stringify")) {
        Buf b;
        if (!allow_keys(t, k_plain, 1, err))
            return 0;
        buf_init(&b);
        json_write(&b, elem);
        *out = str_from_buf(e, &b);
        return 1;
    }
    if (jstr_eq(tv, "type")) {
        if (!allow_keys(t, k_plain, 1, err))
            return 0;
        *out = jv_str(e->a, jv_type_name(elem), strlen(jv_type_name(elem)));
        return 1;
    }
    if (jstr_eq(tv, "op")) {
        JVal *opv, *argsv, *sub;
        int64_t ai, bi, r;
        if (!allow_keys(t, k_op, 3, err))
            return 0;
        opv = jv_obj_get_last(t, "op");
        if (!opv || opv->type != J_STR || !is_math2_op(opv->u.str.s))
            return oerr(err, "INVALID_ARG", "map transform 'op' must be a 2-number op");
        if (!jv_obj_get_last(t, "args"))
            return oerr(err, "INVALID_ARG", "map transform 'op' requires 'args'");
        argsv = jv_obj_get_last(t, "args");
        if (argsv->type != J_OBJ)
            return oerr(err, "INVALID_ARG", "map transform 'args' must be an object");
        sub = subst_self(e, argsv, elem);
        if (!allow_keys(sub, k_ab, 2, err))
            return 0;
        if (!get_int(sub, "a", &ai, err) || !get_int(sub, "b", &bi, err))
            return 0;
        if (!math2(opv->u.str.s, ai, bi, &r, err))
            return 0;
        *out = jv_int(e->a, r);
        return 1;
    }
    return oerr(err, "INVALID_ARG", "unknown map transform type");
}

static int apply_predicate(Engine *e, JVal *elem, JVal *p, int *out, OpErr *err)
{
    static const char *const k_plain[] = {"type"};
    static const char *const k_value[] = {"type", "value"};
    static const char *const k_typeis[] = {"type", "target", "value", "type_name", "name"};
    JVal *tv;
    const char *disc;

    (void)e;
    if (p && p->type == J_STR) {
        if (jstr_eq(p, "even") || jstr_eq(p, "odd")) {
            int64_t ev;
            if (!int_of(elem, &ev))
                return oerr(err, "INVALID_ARG", "even/odd predicate requires an integer");
            *out = (ev % 2 == 0) == jstr_eq(p, "even");
            return 1;
        }
        if (jstr_eq(p, "truthy")) {
            *out = jv_truthy(elem);
            return 1;
        }
        if (jstr_eq(p, "falsy")) {
            *out = !jv_truthy(elem);
            return 1;
        }
        return oerr(err, "INVALID_ARG", "unknown filter predicate");
    }
    if (!p || p->type != J_OBJ)
        return oerr(err, "INVALID_ARG", "filter predicate must be an object");

    /* Discriminator: first "type" that names a known predicate (tolerates the
       spec's {"type":"type_is","type":"number"} spelling with a repeated key). */
    tv = NULL;
    {
        size_t i;
        static const char *const names[] = {"even",     "odd",      "gt",
                                            "gte",      "lt",       "lte",
                                            "eq",       "ne",       "truthy",
                                            "falsy",    "type_is",  "string_contains"};
        for (i = 0; i < p->u.obj.len && !tv; i++) {
            JPair *q = &p->u.obj.pairs[i];
            size_t j;
            if (!pair_key_is(q, "type") || q->val->type != J_STR)
                continue;
            for (j = 0; j < sizeof(names) / sizeof(names[0]); j++) {
                if (jstr_eq(q->val, names[j])) {
                    tv = q->val;
                    break;
                }
            }
        }
        if (!tv)
            tv = jv_obj_get_last(p, "type");
    }
    if (!tv || tv->type != J_STR)
        return oerr(err, "INVALID_ARG", "filter predicate requires string 'type'");
    disc = tv->u.str.s;

    if (strcmp(disc, "even") == 0 || strcmp(disc, "odd") == 0) {
        int64_t ev;
        if (!allow_keys(p, k_plain, 1, err))
            return 0;
        if (!int_of(elem, &ev))
            return oerr(err, "INVALID_ARG", "even/odd predicate requires an integer");
        *out = (ev % 2 == 0) == (strcmp(disc, "even") == 0);
        return 1;
    }
    if (strcmp(disc, "gt") == 0 || strcmp(disc, "gte") == 0 ||
        strcmp(disc, "lt") == 0 || strcmp(disc, "lte") == 0 ||
        strcmp(disc, "eq") == 0 || strcmp(disc, "ne") == 0) {
        JVal val;
        int64_t iv;
        int c;
        if (!allow_keys(p, k_value, 2, err))
            return 0;
        if (!get_int(p, "value", &iv, err))
            return 0;
        memset(&val, 0, sizeof(val));
        val.type = J_NUM;
        val.u.num.is_int = 1;
        val.u.num.fits = 1;
        val.u.num.i = iv;
        val.u.num.d = (double)iv;
        c = jv_cmp(elem, &val);
        if (strcmp(disc, "eq") == 0)
            *out = jv_equal(elem, &val);
        else if (strcmp(disc, "ne") == 0)
            *out = !jv_equal(elem, &val);
        else if (strcmp(disc, "gt") == 0)
            *out = c > 0;
        else if (strcmp(disc, "gte") == 0)
            *out = c >= 0;
        else if (strcmp(disc, "lt") == 0)
            *out = c < 0;
        else
            *out = c <= 0;
        return 1;
    }
    if (strcmp(disc, "truthy") == 0) {
        if (!allow_keys(p, k_plain, 1, err))
            return 0;
        *out = jv_truthy(elem);
        return 1;
    }
    if (strcmp(disc, "falsy") == 0) {
        if (!allow_keys(p, k_plain, 1, err))
            return 0;
        *out = !jv_truthy(elem);
        return 1;
    }
    if (strcmp(disc, "type_is") == 0) {
        const char *target = NULL;
        size_t tn = 0, i;
        if (!allow_keys(p, k_typeis, 5, err))
            return 0;
        for (i = 0; i < p->u.obj.len; i++) {
            JPair *q = &p->u.obj.pairs[i];
            JVal *v = q->val;
            if (v->type != J_STR)
                continue;
            if (!(pair_key_is(q, "type") || pair_key_is(q, "target") ||
                  pair_key_is(q, "value") || pair_key_is(q, "type_name") ||
                  pair_key_is(q, "name")))
                continue;
            if (is_valid_type_name(v->u.str.s, v->u.str.len)) {
                target = v->u.str.s;
                tn = v->u.str.len;
                break;
            }
        }
        if (!target)
            return oerr(err, "INVALID_ARG", "type_is predicate requires a type name");
        {
            const char *actual = jv_type_name(elem);
            *out = strlen(actual) == tn && memcmp(actual, target, tn) == 0;
        }
        return 1;
    }
    if (strcmp(disc, "string_contains") == 0) {
        const char *needle;
        size_t nn;
        if (!allow_keys(p, k_value, 2, err))
            return 0;
        if (!get_str(p, "value", &needle, &nn, err))
            return 0;
        if (elem->type != J_STR)
            return oerr(err, "INVALID_ARG", "string_contains predicate requires a string");
        *out = contains_sub(elem->u.str.s, elem->u.str.len, needle, nn);
        return 1;
    }
    return oerr(err, "INVALID_ARG", "unknown filter predicate type");
}

/* ================================================================== */
/* Sorting helpers                                                     */
/* ================================================================== */

static void merge_sort(JVal **a, JVal **tmp, size_t lo, size_t hi)
{
    size_t mid, i, j, k;
    if (hi - lo < 2)
        return;
    mid = lo + (hi - lo) / 2;
    merge_sort(a, tmp, lo, mid);
    merge_sort(a, tmp, mid, hi);
    i = lo;
    j = mid;
    k = lo;
    while (i < mid && j < hi) {
        if (jv_cmp(a[j], a[i]) < 0)
            tmp[k++] = a[j++];
        else
            tmp[k++] = a[i++];
    }
    while (i < mid)
        tmp[k++] = a[i++];
    while (j < hi)
        tmp[k++] = a[j++];
    for (i = lo; i < hi; i++)
        a[i] = tmp[i];
}

static void merge_sort_idx(JVal **vals, size_t *idx, size_t *tmp, size_t lo, size_t hi)
{
    size_t mid, i, j, k;
    if (hi - lo < 2)
        return;
    mid = lo + (hi - lo) / 2;
    merge_sort_idx(vals, idx, tmp, lo, mid);
    merge_sort_idx(vals, idx, tmp, mid, hi);
    i = lo;
    j = mid;
    k = lo;
    while (i < mid && j < hi) {
        if (jv_cmp(vals[idx[j]], vals[idx[i]]) < 0)
            tmp[k++] = idx[j++];
        else
            tmp[k++] = idx[i++];
    }
    while (i < mid)
        tmp[k++] = idx[i++];
    while (j < hi)
        tmp[k++] = idx[j++];
    for (i = lo; i < hi; i++)
        idx[i] = tmp[i];
}

static int cmp_size_asc(const void *x, const void *y)
{
    size_t a = *(const size_t *)x;
    size_t b = *(const size_t *)y;
    return a < b ? -1 : (a > b ? 1 : 0);
}

/* ================================================================== */
/* Operations                                                          */
/* ================================================================== */

static int run_op(Engine *e, Step *s, JVal *args, JVal **out, OpErr *err)
{
    const char *op = s->op->u.str.s;

    if (strcmp(op, "add") == 0 || strcmp(op, "subtract") == 0 ||
        strcmp(op, "multiply") == 0 || strcmp(op, "divide") == 0 ||
        strcmp(op, "mod") == 0) {
        static const char *const keys[] = {"a", "b"};
        int64_t a, b, r;
        if (!allow_keys(args, keys, 2, err))
            return 0;
        if (!get_int(args, "a", &a, err) || !get_int(args, "b", &b, err))
            return 0;
        if (!math2(op, a, b, &r, err))
            return 0;
        *out = jv_int(e->a, r);
        return 1;
    }

    if (strcmp(op, "sum") == 0 || strcmp(op, "min") == 0 || strcmp(op, "max") == 0) {
        static const char *const keys[] = {"input"};
        JVal *in;
        size_t i;
        int64_t acc = 0, v;
        if (!allow_keys(args, keys, 1, err))
            return 0;
        if (!get_arr(args, "input", &in, err))
            return 0;
        if (in->u.arr.len == 0) {
            if (strcmp(op, "sum") == 0) {
                *out = jv_int(e->a, 0);
                return 1;
            }
            return oerr(err, "EMPTY_ARRAY", "%s of empty array", op);
        }
        for (i = 0; i < in->u.arr.len; i++) {
            if (!int_of(in->u.arr.items[i], &v))
                return oerr(err, "INVALID_ARG", "array elements must be i64 integers");
            if (i == 0) {
                acc = v;
            } else if (strcmp(op, "sum") == 0) {
                if (__builtin_add_overflow(acc, v, &acc))
                    return oerr(err, "ARITHMETIC_OVERFLOW", "sum overflow");
            } else if (strcmp(op, "min") == 0) {
                if (v < acc)
                    acc = v;
            } else {
                if (v > acc)
                    acc = v;
            }
        }
        *out = jv_int(e->a, acc);
        return 1;
    }

    if (strcmp(op, "range") == 0) {
        static const char *const keys[] = {"start", "end"};
        int64_t start, end, v;
        JVal *arr;
        if (!allow_keys(args, keys, 2, err))
            return 0;
        if (!get_int(args, "start", &start, err) || !get_int(args, "end", &end, err))
            return 0;
        arr = jv_arr(e->a);
        if (start <= end) {
            v = start;
            while (1) {
                jv_arr_push(e->a, arr, jv_int(e->a, v));
                if (v == end)
                    break;
                v++;
            }
        }
        *out = arr;
        return 1;
    }

    if (strcmp(op, "concat") == 0) {
        static const char *const keys[] = {"a", "b"};
        const char *a, *b;
        size_t an, bn;
        Buf buf;
        if (!allow_keys(args, keys, 2, err))
            return 0;
        if (!get_str(args, "a", &a, &an, err) || !get_str(args, "b", &b, &bn, err))
            return 0;
        buf_init(&buf);
        buf_append(&buf, a, an);
        buf_append(&buf, b, bn);
        *out = str_from_buf(e, &buf);
        return 1;
    }

    if (strcmp(op, "split") == 0) {
        static const char *const keys[] = {"input", "sep"};
        const char *in, *sep;
        size_t inn, sepn, i;
        JVal *arr;
        if (!allow_keys(args, keys, 2, err))
            return 0;
        if (!get_str(args, "input", &in, &inn, err) ||
            !get_str(args, "sep", &sep, &sepn, err))
            return 0;
        arr = jv_arr(e->a);
        if (sepn == 0) {
            i = 0;
            while (i < inn) {
                size_t cl = utf8_char_len(in + i, inn - i);
                jv_arr_push(e->a, arr, jv_str(e->a, in + i, cl));
                i += cl;
            }
        } else {
            size_t start = 0;
            i = 0;
            while (i + sepn <= inn) {
                if (memcmp(in + i, sep, sepn) == 0) {
                    jv_arr_push(e->a, arr, jv_str(e->a, in + start, i - start));
                    i += sepn;
                    start = i;
                } else {
                    i++;
                }
            }
            jv_arr_push(e->a, arr, jv_str(e->a, in + start, inn - start));
        }
        *out = arr;
        return 1;
    }

    if (strcmp(op, "join") == 0) {
        static const char *const keys[] = {"input", "sep"};
        JVal *in;
        const char *sep;
        size_t sepn, i;
        Buf buf;
        if (!allow_keys(args, keys, 2, err))
            return 0;
        if (!get_arr(args, "input", &in, err) ||
            !get_str(args, "sep", &sep, &sepn, err))
            return 0;
        buf_init(&buf);
        for (i = 0; i < in->u.arr.len; i++) {
            JVal *v = in->u.arr.items[i];
            if (v->type != J_STR) {
                buf_free(&buf);
                return oerr(err, "INVALID_ARG", "join input must be strings");
            }
            if (i)
                buf_append(&buf, sep, sepn);
            buf_append(&buf, v->u.str.s, v->u.str.len);
        }
        *out = str_from_buf(e, &buf);
        return 1;
    }

    if (strcmp(op, "uppercase") == 0 || strcmp(op, "lowercase") == 0) {
        static const char *const keys[] = {"input"};
        const char *in;
        size_t inn, i;
        char *buf;
        JVal *v;
        if (!allow_keys(args, keys, 1, err))
            return 0;
        if (!get_str(args, "input", &in, &inn, err))
            return 0;
        buf = (char *)arena_alloc(e->a, inn + 1);
        for (i = 0; i < inn; i++) {
            char c = in[i];
            if (strcmp(op, "uppercase") == 0) {
                if (c >= 'a' && c <= 'z')
                    c = (char)(c - 'a' + 'A');
            } else {
                if (c >= 'A' && c <= 'Z')
                    c = (char)(c - 'A' + 'a');
            }
            buf[i] = c;
        }
        buf[inn] = '\0';
        v = jv_str(e->a, buf, inn);
        *out = v;
        return 1;
    }

    if (strcmp(op, "trim") == 0) {
        static const char *const keys[] = {"input"};
        const char *in;
        size_t inn, lo = 0, hi;
        if (!allow_keys(args, keys, 1, err))
            return 0;
        if (!get_str(args, "input", &in, &inn, err))
            return 0;
        hi = inn;
        while (lo < hi) {
            char c = in[lo];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v')
                lo++;
            else
                break;
        }
        while (hi > lo) {
            char c = in[hi - 1];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v')
                hi--;
            else
                break;
        }
        *out = jv_str(e->a, in + lo, hi - lo);
        return 1;
    }

    if (strcmp(op, "replace") == 0) {
        static const char *const keys[] = {"input", "search", "replace"};
        const char *in, *search, *repl;
        size_t inn, sn, rn, i;
        Buf buf;
        if (!allow_keys(args, keys, 3, err))
            return 0;
        if (!get_str(args, "input", &in, &inn, err) ||
            !get_str(args, "search", &search, &sn, err) ||
            !get_str(args, "replace", &repl, &rn, err))
            return 0;
        if (sn == 0)
            return oerr(err, "INVALID_ARG", "replace search must not be empty");
        buf_init(&buf);
        i = 0;
        while (i < inn) {
            if (i + sn <= inn && memcmp(in + i, search, sn) == 0) {
                buf_append(&buf, repl, rn);
                i += sn;
            } else {
                buf_putc(&buf, in[i]);
                i++;
            }
        }
        *out = str_from_buf(e, &buf);
        return 1;
    }

    if (strcmp(op, "length") == 0) {
        static const char *const keys[] = {"input"};
        const char *in;
        size_t inn, i = 0;
        int64_t count = 0;
        if (!allow_keys(args, keys, 1, err))
            return 0;
        if (!get_str(args, "input", &in, &inn, err))
            return 0;
        while (i < inn) {
            i += utf8_char_len(in + i, inn - i);
            count++;
        }
        *out = jv_int(e->a, count);
        return 1;
    }

    if (strcmp(op, "map") == 0) {
        static const char *const keys[] = {"input", "transform"};
        JVal *in, *t, *arr;
        size_t i;
        if (!allow_keys(args, keys, 2, err))
            return 0;
        if (!get_arr(args, "input", &in, err))
            return 0;
        if (!get_any(args, "transform", &t, err))
            return 0;
        arr = jv_arr(e->a);
        for (i = 0; i < in->u.arr.len; i++) {
            JVal *r = NULL;
            if (!apply_transform(e, in->u.arr.items[i], t, &r, err))
                return 0;
            if (!r)
                r = jv_null(e->a);
            jv_arr_push(e->a, arr, r);
        }
        *out = arr;
        return 1;
    }

    if (strcmp(op, "filter") == 0) {
        static const char *const keys[] = {"input", "predicate"};
        JVal *in, *p, *arr;
        size_t i;
        if (!allow_keys(args, keys, 2, err))
            return 0;
        if (!get_arr(args, "input", &in, err))
            return 0;
        if (!get_any(args, "predicate", &p, err))
            return 0;
        arr = jv_arr(e->a);
        for (i = 0; i < in->u.arr.len; i++) {
            int keep = 0;
            if (!apply_predicate(e, in->u.arr.items[i], p, &keep, err))
                return 0;
            if (keep)
                jv_arr_push(e->a, arr, in->u.arr.items[i]);
        }
        *out = arr;
        return 1;
    }

    if (strcmp(op, "reduce") == 0) {
        static const char *const keys[] = {"input", "op", "initial"};
        JVal *in, *opv;
        int64_t acc = 0, v;
        int has_initial = 0;
        size_t i;
        if (!allow_keys(args, keys, 3, err))
            return 0;
        if (!get_arr(args, "input", &in, err))
            return 0;
        opv = jv_obj_get_last(args, "op");
        if (!opv)
            return oerr(err, "INVALID_ARG", "missing argument 'op'");
        if (opv->type != J_STR || !(jstr_eq(opv, "add") || jstr_eq(opv, "subtract") ||
                                    jstr_eq(opv, "multiply")))
            return oerr(err, "INVALID_ARG", "reduce op must be add, subtract or multiply");
        if (!get_int_opt(args, "initial", &has_initial, &acc, err))
            return 0;
        if (!has_initial)
            acc = jstr_eq(opv, "multiply") ? 1 : 0;
        for (i = 0; i < in->u.arr.len; i++) {
            int64_t r;
            if (!int_of(in->u.arr.items[i], &v))
                return oerr(err, "INVALID_ARG", "reduce input must be i64 integers");
            if (!math2(opv->u.str.s, acc, v, &r, err))
                return 0;
            acc = r;
        }
        *out = jv_int(e->a, acc);
        return 1;
    }

    if (strcmp(op, "sort") == 0) {
        static const char *const keys[] = {"input"};
        JVal *in, *arr;
        JVal **items, **tmp;
        size_t i;
        if (!allow_keys(args, keys, 1, err))
            return 0;
        if (!get_arr(args, "input", &in, err))
            return 0;
        items = (JVal **)malloc((in->u.arr.len ? in->u.arr.len : 1) * sizeof(JVal *));
        tmp = (JVal **)malloc((in->u.arr.len ? in->u.arr.len : 1) * sizeof(JVal *));
        if (!items || !tmp) {
            free(items);
            free(tmp);
            return oerr(err, "INTERNAL_ERROR", "out of memory");
        }
        for (i = 0; i < in->u.arr.len; i++)
            items[i] = in->u.arr.items[i];
        merge_sort(items, tmp, 0, in->u.arr.len);
        arr = jv_arr(e->a);
        for (i = 0; i < in->u.arr.len; i++)
            jv_arr_push(e->a, arr, items[i]);
        free(items);
        free(tmp);
        *out = arr;
        return 1;
    }

    if (strcmp(op, "reverse") == 0) {
        static const char *const keys[] = {"input"};
        JVal *in, *arr;
        size_t i;
        if (!allow_keys(args, keys, 1, err))
            return 0;
        if (!get_arr(args, "input", &in, err))
            return 0;
        arr = jv_arr(e->a);
        for (i = in->u.arr.len; i > 0; i--)
            jv_arr_push(e->a, arr, in->u.arr.items[i - 1]);
        *out = arr;
        return 1;
    }

    if (strcmp(op, "unique") == 0) {
        static const char *const keys[] = {"input"};
        JVal *in, *arr;
        size_t n, i, pn = 0;
        size_t *idx, *tmp, *picked;
        if (!allow_keys(args, keys, 1, err))
            return 0;
        if (!get_arr(args, "input", &in, err))
            return 0;
        n = in->u.arr.len;
        arr = jv_arr(e->a);
        if (n == 0) {
            *out = arr;
            return 1;
        }
        idx = (size_t *)malloc(n * sizeof(size_t));
        tmp = (size_t *)malloc(n * sizeof(size_t));
        picked = (size_t *)malloc(n * sizeof(size_t));
        if (!idx || !tmp || !picked) {
            free(idx);
            free(tmp);
            free(picked);
            return oerr(err, "INTERNAL_ERROR", "out of memory");
        }
        for (i = 0; i < n; i++)
            idx[i] = i;
        merge_sort_idx(in->u.arr.items, idx, tmp, 0, n);
        for (i = 0; i < n;) {
            size_t j = i + 1;
            size_t best = idx[i];
            while (j < n && jv_cmp(in->u.arr.items[idx[i]], in->u.arr.items[idx[j]]) == 0) {
                if (idx[j] < best)
                    best = idx[j];
                j++;
            }
            picked[pn++] = best;
            i = j;
        }
        qsort(picked, pn, sizeof(size_t), cmp_size_asc);
        for (i = 0; i < pn; i++)
            jv_arr_push(e->a, arr, in->u.arr.items[picked[i]]);
        free(idx);
        free(tmp);
        free(picked);
        *out = arr;
        return 1;
    }

    if (strcmp(op, "slice") == 0) {
        static const char *const keys[] = {"input", "start", "end"};
        JVal *in, *arr;
        int64_t start, endv, L, s, t;
        int has_end = 0;
        size_t i;
        if (!allow_keys(args, keys, 3, err))
            return 0;
        if (!get_arr(args, "input", &in, err))
            return 0;
        if (!get_int(args, "start", &start, err))
            return 0;
        if (!get_int_opt(args, "end", &has_end, &endv, err))
            return 0;
        L = (int64_t)in->u.arr.len;
        s = start;
        if (s < 0) {
            s += L;
            if (s < 0)
                s = 0;
        }
        if (s > L)
            s = L;
        if (has_end) {
            t = endv;
            if (t < 0) {
                t += L;
                if (t < 0)
                    t = 0;
            }
            if (t > L)
                t = L;
        } else {
            t = L;
        }
        arr = jv_arr(e->a);
        if (s < t) {
            for (i = (size_t)s; i < (size_t)t; i++)
                jv_arr_push(e->a, arr, in->u.arr.items[i]);
        }
        *out = arr;
        return 1;
    }

    if (strcmp(op, "flatten") == 0) {
        static const char *const keys[] = {"input"};
        JVal *in, *arr;
        size_t i, j;
        if (!allow_keys(args, keys, 1, err))
            return 0;
        if (!get_arr(args, "input", &in, err))
            return 0;
        arr = jv_arr(e->a);
        for (i = 0; i < in->u.arr.len; i++) {
            JVal *v = in->u.arr.items[i];
            if (v->type == J_ARR) {
                for (j = 0; j < v->u.arr.len; j++)
                    jv_arr_push(e->a, arr, v->u.arr.items[j]);
            } else {
                jv_arr_push(e->a, arr, v);
            }
        }
        *out = arr;
        return 1;
    }

    if (strcmp(op, "get") == 0) {
        static const char *const keys[] = {"input", "key"};
        JVal *in, *v;
        const char *key;
        size_t kn;
        if (!allow_keys(args, keys, 2, err))
            return 0;
        if (!get_obj(args, "input", &in, err) ||
            !get_str(args, "key", &key, &kn, err))
            return 0;
        v = jv_obj_get_n(in, key, kn);
        *out = v ? v : jv_null(e->a);
        return 1;
    }

    if (strcmp(op, "set") == 0) {
        static const char *const keys[] = {"input", "key", "value"};
        JVal *in, *v, *o;
        const char *key;
        size_t kn;
        if (!allow_keys(args, keys, 3, err))
            return 0;
        if (!get_obj(args, "input", &in, err) ||
            !get_str(args, "key", &key, &kn, err) ||
            !get_any(args, "value", &v, err))
            return 0;
        o = obj_copy(e, in);
        jv_obj_set(e->a, o, key, kn, v);
        *out = o;
        return 1;
    }

    if (strcmp(op, "keys") == 0 || strcmp(op, "values") == 0) {
        static const char *const keys[] = {"input"};
        JVal *in, *arr;
        size_t *idx;
        size_t i;
        if (!allow_keys(args, keys, 1, err))
            return 0;
        if (!get_obj(args, "input", &in, err))
            return 0;
        idx = (size_t *)malloc((in->u.obj.len ? in->u.obj.len : 1) * sizeof(size_t));
        if (!idx)
            return oerr(err, "INTERNAL_ERROR", "out of memory");
        for (i = 0; i < in->u.obj.len; i++)
            idx[i] = i;
        for (i = 1; i < in->u.obj.len; i++) {
            size_t key = idx[i];
            size_t j = i;
            while (j > 0) {
                JPair *pa = &in->u.obj.pairs[idx[j - 1]];
                JPair *pb = &in->u.obj.pairs[key];
                if (cmp_bytes(pa->key, pa->klen, pb->key, pb->klen) <= 0)
                    break;
                idx[j] = idx[j - 1];
                j--;
            }
            idx[j] = key;
        }
        arr = jv_arr(e->a);
        for (i = 0; i < in->u.obj.len; i++) {
            JPair *p = &in->u.obj.pairs[idx[i]];
            if (strcmp(op, "keys") == 0)
                jv_arr_push(e->a, arr, jv_str(e->a, p->key, p->klen));
            else
                jv_arr_push(e->a, arr, p->val);
        }
        free(idx);
        *out = arr;
        return 1;
    }

    if (strcmp(op, "merge") == 0) {
        static const char *const keys[] = {"a", "b"};
        JVal *a, *b, *o;
        size_t i;
        if (!allow_keys(args, keys, 2, err))
            return 0;
        if (!get_obj(args, "a", &a, err) || !get_obj(args, "b", &b, err))
            return 0;
        o = obj_copy(e, a);
        for (i = 0; i < b->u.obj.len; i++)
            jv_obj_set(e->a, o, b->u.obj.pairs[i].key, b->u.obj.pairs[i].klen,
                       b->u.obj.pairs[i].val);
        *out = o;
        return 1;
    }

    if (strcmp(op, "pick") == 0 || strcmp(op, "omit") == 0) {
        static const char *const keys[] = {"input", "keys"};
        JVal *in, *ks, *o;
        size_t i, j;
        if (!allow_keys(args, keys, 2, err))
            return 0;
        if (!get_obj(args, "input", &in, err) || !get_arr(args, "keys", &ks, err))
            return 0;
        for (i = 0; i < ks->u.arr.len; i++) {
            if (ks->u.arr.items[i]->type != J_STR)
                return oerr(err, "INVALID_ARG", "keys must be strings");
        }
        o = jv_obj(e->a);
        if (strcmp(op, "omit") == 0) {
            for (i = 0; i < in->u.obj.len; i++) {
                JPair *p = &in->u.obj.pairs[i];
                int skip = 0;
                for (j = 0; j < ks->u.arr.len; j++) {
                    JVal *k = ks->u.arr.items[j];
                    if (p->klen == k->u.str.len &&
                        (p->klen == 0 || memcmp(p->key, k->u.str.s, p->klen) == 0)) {
                        skip = 1;
                        break;
                    }
                }
                if (!skip)
                    jv_obj_append(e->a, o, p->key, p->klen, p->val);
            }
        } else {
            /* unique, then sorted by bytes */
            const char **names = (const char **)arena_alloc(
                e->a, (ks->u.arr.len ? ks->u.arr.len : 1) * sizeof(char *));
            size_t *lens = (size_t *)arena_alloc(
                e->a, (ks->u.arr.len ? ks->u.arr.len : 1) * sizeof(size_t));
            size_t count = 0;
            for (i = 0; i < ks->u.arr.len; i++) {
                JVal *k = ks->u.arr.items[i];
                int dup = 0;
                for (j = 0; j < count; j++) {
                    if (lens[j] == k->u.str.len &&
                        (lens[j] == 0 || memcmp(names[j], k->u.str.s, lens[j]) == 0)) {
                        dup = 1;
                        break;
                    }
                }
                if (!dup) {
                    names[count] = k->u.str.s;
                    lens[count] = k->u.str.len;
                    count++;
                }
            }
            for (i = 1; i < count; i++) {
                const char *kn = names[i];
                size_t kl = lens[i];
                size_t q = i;
                while (q > 0 && cmp_bytes(names[q - 1], lens[q - 1], kn, kl) > 0) {
                    names[q] = names[q - 1];
                    lens[q] = lens[q - 1];
                    q--;
                }
                names[q] = kn;
                lens[q] = kl;
            }
            for (i = 0; i < count; i++) {
                JVal *v = jv_obj_get_n(in, names[i], lens[i]);
                jv_obj_append(e->a, o, names[i], lens[i], v ? v : jv_null(e->a));
            }
        }
        *out = o;
        return 1;
    }

    if (strcmp(op, "constant") == 0) {
        static const char *const keys[] = {"value"};
        JVal *v;
        if (!allow_keys(args, keys, 1, err))
            return 0;
        if (!get_any(args, "value", &v, err))
            return 0;
        *out = v;
        return 1;
    }

    if (strcmp(op, "equals") == 0) {
        static const char *const keys[] = {"a", "b"};
        JVal *a, *b;
        if (!allow_keys(args, keys, 2, err))
            return 0;
        if (!get_any(args, "a", &a, err) || !get_any(args, "b", &b, err))
            return 0;
        *out = jv_bool(e->a, jv_equal(a, b));
        return 1;
    }

    if (strcmp(op, "compare") == 0) {
        static const char *const keys[] = {"a", "b", "op"};
        JVal *a, *b, *opv;
        int c;
        if (!allow_keys(args, keys, 3, err))
            return 0;
        if (!get_any(args, "a", &a, err) || !get_any(args, "b", &b, err))
            return 0;
        opv = jv_obj_get_last(args, "op");
        if (!opv)
            return oerr(err, "INVALID_ARG", "missing argument 'op'");
        if (opv->type != J_STR)
            return oerr(err, "INVALID_ARG", "compare op must be a string");
        c = jv_cmp(a, b);
        if (jstr_eq(opv, "gt"))
            *out = jv_bool(e->a, c > 0);
        else if (jstr_eq(opv, "gte"))
            *out = jv_bool(e->a, c >= 0);
        else if (jstr_eq(opv, "lt"))
            *out = jv_bool(e->a, c < 0);
        else if (jstr_eq(opv, "lte"))
            *out = jv_bool(e->a, c <= 0);
        else
            return oerr(err, "INVALID_ARG", "compare op must be gt, gte, lt or lte");
        return 1;
    }

    if (strcmp(op, "and") == 0 || strcmp(op, "or") == 0) {
        static const char *const keys[] = {"a", "b"};
        JVal *a, *b;
        int ta, tb;
        if (!allow_keys(args, keys, 2, err))
            return 0;
        if (!get_any(args, "a", &a, err) || !get_any(args, "b", &b, err))
            return 0;
        ta = jv_truthy(a);
        tb = jv_truthy(b);
        *out = jv_bool(e->a, strcmp(op, "and") == 0 ? (ta && tb) : (ta || tb));
        return 1;
    }

    if (strcmp(op, "not") == 0) {
        static const char *const keys[] = {"a"};
        JVal *a;
        if (!allow_keys(args, keys, 1, err))
            return 0;
        if (!get_any(args, "a", &a, err))
            return 0;
        *out = jv_bool(e->a, !jv_truthy(a));
        return 1;
    }

    if (strcmp(op, "type") == 0) {
        static const char *const keys[] = {"value"};
        JVal *v;
        const char *tn;
        if (!allow_keys(args, keys, 1, err))
            return 0;
        if (!get_any(args, "value", &v, err))
            return 0;
        tn = jv_type_name(v);
        *out = jv_str(e->a, tn, strlen(tn));
        return 1;
    }

    if (strcmp(op, "fail_until") == 0) {
        static const char *const keys[] = {"failures", "value"};
        int64_t failures;
        JVal *v;
        if (!allow_keys(args, keys, 2, err))
            return 0;
        if (!get_int(args, "failures", &failures, err))
            return 0;
        if (failures < 0 || failures > 10)
            return oerr(err, "INVALID_ARG", "failures must be in 0..10");
        if (!get_any(args, "value", &v, err))
            return 0;
        if ((int64_t)s->attempts <= failures)
            return oerr(err, "TRANSIENT_FAILURE", "transient failure on attempt %d",
                        s->attempts);
        *out = v;
        return 1;
    }

    return oerr(err, "INVALID_ARG", "unknown operation '%s'", op);
}

/* ================================================================== */
/* References                                                          */
/* ================================================================== */

static int find_step(Engine *e, const char *id, size_t idlen)
{
    int i;
    for (i = 0; i < e->n; i++) {
        Step *s = &e->steps[i];
        if (s->idlen == idlen && (idlen == 0 || memcmp(s->id, id, idlen) == 0))
            return i;
    }
    return -1;
}

static int is_transitive_dep(Engine *e, int from, int target)
{
    unsigned char *visited;
    int *stack;
    int sp = 0;
    int found = 0;
    int i;

    if (from == target)
        return 0;
    visited = (unsigned char *)calloc((size_t)e->n, 1);
    stack = (int *)malloc((size_t)e->n * sizeof(int));
    if (!visited || !stack) {
        free(visited);
        free(stack);
        return 0;
    }
    stack[sp++] = from;
    visited[from] = 1;
    while (sp > 0 && !found) {
        int cur = stack[--sp];
        Step *s = &e->steps[cur];
        for (i = 0; i < s->ndeps; i++) {
            int d = s->deps[i];
            if (d == target) {
                found = 1;
                break;
            }
            if (!visited[d]) {
                visited[d] = 1;
                stack[sp++] = d;
            }
        }
    }
    free(visited);
    free(stack);
    return found;
}

static int resolve_ref(Engine *e, int sidx, const char *ref, size_t reflen,
                       JVal **out, OpErr *err)
{
    size_t idlen = 0;
    int target;
    JVal *cur;
    size_t pos;

    while (idlen < reflen && ref[idlen] != '.')
        idlen++;
    if (idlen == 0)
        return oerr(err, "INVALID_REFERENCE", "empty reference");
    target = find_step(e, ref, idlen);
    if (target < 0)
        return oerr(err, "INVALID_REFERENCE", "reference to unknown step '%.*s'",
                    (int)idlen, ref);
    if (target == sidx || !is_transitive_dep(e, sidx, target))
        return oerr(err, "INVALID_REFERENCE",
                    "reference to non-dependency step '%.*s'", (int)idlen, ref);
    if (e->steps[target].skipped)
        return oerr(err, "SKIPPED_REFERENCE", "reference to skipped step '%.*s'",
                    (int)idlen, ref);
    if (!e->steps[target].done)
        return oerr(err, "INVALID_REFERENCE", "reference to unfinished step '%.*s'",
                    (int)idlen, ref);

    cur = e->steps[target].result;
    pos = idlen;
    while (pos < reflen) {
        size_t seglen;
        const char *seg;
        pos++; /* skip '.' */
        seg = ref + pos;
        seglen = 0;
        while (pos + seglen < reflen && seg[seglen] != '.')
            seglen++;
        if (seglen == 0)
            return oerr(err, "INVALID_REFERENCE", "empty path segment in reference");
        if (cur->type == J_OBJ) {
            JVal *v = jv_obj_get_n(cur, seg, seglen);
            if (!v)
                return oerr(err, "INVALID_REFERENCE", "missing key '%.*s' in reference",
                            (int)seglen, seg);
            cur = v;
        } else if (cur->type == J_ARR) {
            uint64_t idx = 0;
            size_t k;
            int overflow = 0;
            for (k = 0; k < seglen; k++) {
                if (seg[k] < '0' || seg[k] > '9')
                    return oerr(err, "INVALID_REFERENCE",
                                "non-numeric array index '%.*s'", (int)seglen, seg);
                if (idx > (UINT64_MAX - (uint64_t)(seg[k] - '0')) / 10) {
                    overflow = 1;
                    break;
                }
                idx = idx * 10 + (uint64_t)(seg[k] - '0');
            }
            if (overflow || idx >= (uint64_t)cur->u.arr.len)
                return oerr(err, "INVALID_REFERENCE", "array index out of bounds '%.*s'",
                            (int)seglen, seg);
            cur = cur->u.arr.items[idx];
        } else {
            return oerr(err, "INVALID_REFERENCE",
                        "cannot traverse into non-container value");
        }
        pos += seglen;
    }
    *out = cur;
    return 1;
}

static int resolve_value(Engine *e, int sidx, JVal *v, JVal **out, OpErr *err)
{
    size_t i;
    if (v->type == J_OBJ) {
        if (v->u.obj.len == 1 && pair_key_is(&v->u.obj.pairs[0], "$ref") &&
            v->u.obj.pairs[0].val->type == J_STR) {
            JVal *ref = v->u.obj.pairs[0].val;
            return resolve_ref(e, sidx, ref->u.str.s, ref->u.str.len, out, err);
        }
        {
            JVal *o = jv_obj(e->a);
            for (i = 0; i < v->u.obj.len; i++) {
                JVal *rv;
                if (!resolve_value(e, sidx, v->u.obj.pairs[i].val, &rv, err))
                    return 0;
                jv_obj_append(e->a, o, v->u.obj.pairs[i].key, v->u.obj.pairs[i].klen,
                              rv);
            }
            *out = o;
            return 1;
        }
    }
    if (v->type == J_ARR) {
        JVal *arr = jv_arr(e->a);
        for (i = 0; i < v->u.arr.len; i++) {
            JVal *rv;
            if (!resolve_value(e, sidx, v->u.arr.items[i], &rv, err))
                return 0;
            jv_arr_push(e->a, arr, rv);
        }
        *out = arr;
        return 1;
    }
    *out = v;
    return 1;
}

/* ================================================================== */
/* Conditions                                                          */
/* ================================================================== */

static int eval_cond(Engine *e, int sidx, JVal *c, int *out, OpErr *err)
{
    JVal *opv = jv_obj_get_last(c, "op");
    if (!opv || opv->type != J_STR)
        return oerr(err, "INVALID_CONDITION", "condition without op");

    if (jstr_eq(opv, "all") || jstr_eq(opv, "any")) {
        JVal *conds = jv_obj_get_last(c, "conditions");
        size_t i;
        int is_all = jstr_eq(opv, "all");
        *out = is_all ? 1 : 0;
        for (i = 0; i < conds->u.arr.len; i++) {
            int b;
            if (!eval_cond(e, sidx, conds->u.arr.items[i], &b, err))
                return 0;
            if (is_all && !b) {
                *out = 0;
                return 1;
            }
            if (!is_all && b) {
                *out = 1;
                return 1;
            }
        }
        return 1;
    }
    if (jstr_eq(opv, "not")) {
        int b;
        if (!eval_cond(e, sidx, jv_obj_get_last(c, "condition"), &b, err))
            return 0;
        *out = !b;
        return 1;
    }
    if (jstr_eq(opv, "eq") || jstr_eq(opv, "ne") || jstr_eq(opv, "gt") ||
        jstr_eq(opv, "gte") || jstr_eq(opv, "lt") || jstr_eq(opv, "lte")) {
        JVal *lv, *rv;
        int cmp;
        if (!resolve_value(e, sidx, jv_obj_get_last(c, "left"), &lv, err) ||
            !resolve_value(e, sidx, jv_obj_get_last(c, "right"), &rv, err))
            return 0;
        if (jstr_eq(opv, "eq")) {
            *out = jv_equal(lv, rv);
            return 1;
        }
        if (jstr_eq(opv, "ne")) {
            *out = !jv_equal(lv, rv);
            return 1;
        }
        cmp = jv_cmp(lv, rv);
        if (jstr_eq(opv, "gt"))
            *out = cmp > 0;
        else if (jstr_eq(opv, "gte"))
            *out = cmp >= 0;
        else if (jstr_eq(opv, "lt"))
            *out = cmp < 0;
        else
            *out = cmp <= 0;
        return 1;
    }
    return oerr(err, "INVALID_CONDITION", "unknown condition op");
}

/* ================================================================== */
/* Validation                                                          */
/* ================================================================== */

static int valid_ident(const char *s, size_t n)
{
    size_t i;
    if (n == 0)
        return 0;
    if (!((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z') || s[0] == '_'))
        return 0;
    for (i = 1; i < n; i++) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_'))
            return 0;
    }
    return 1;
}

static int known_op(const JVal *op)
{
    static const char *const names[] = {
        "add",       "subtract", "multiply", "divide",   "mod",
        "sum",       "min",      "max",      "range",    "concat",
        "split",     "join",     "uppercase","lowercase","trim",
        "replace",   "length",   "map",      "filter",   "reduce",
        "sort",      "reverse",  "unique",   "slice",    "flatten",
        "get",       "set",      "keys",     "values",   "merge",
        "pick",      "omit",     "constant", "equals",   "compare",
        "and",       "or",       "not",      "type",     "fail_until"};
    size_t i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (jstr_eq(op, names[i]))
            return 1;
    }
    return 0;
}

static int cond_key_allowed(const JPair *p, int kind)
{
    if (pair_key_is(p, "op"))
        return 1;
    if (kind == 0)
        return pair_key_is(p, "left") || pair_key_is(p, "right");
    if (kind == 1)
        return pair_key_is(p, "conditions");
    return pair_key_is(p, "condition");
}

static int validate_cond(JVal *c, const char **msg)
{
    JVal *opv;
    int kind;
    size_t i;

    if (!c || c->type != J_OBJ) {
        *msg = "when must be an object";
        return 0;
    }
    opv = jv_obj_get_last(c, "op");
    if (!opv || opv->type != J_STR) {
        *msg = "condition requires a string 'op'";
        return 0;
    }
    if (jstr_eq(opv, "eq") || jstr_eq(opv, "ne") || jstr_eq(opv, "gt") ||
        jstr_eq(opv, "gte") || jstr_eq(opv, "lt") || jstr_eq(opv, "lte"))
        kind = 0;
    else if (jstr_eq(opv, "all") || jstr_eq(opv, "any"))
        kind = 1;
    else if (jstr_eq(opv, "not"))
        kind = 2;
    else {
        *msg = "unknown condition op";
        return 0;
    }
    for (i = 0; i < c->u.obj.len; i++) {
        if (!cond_key_allowed(&c->u.obj.pairs[i], kind)) {
            *msg = "unknown field in condition";
            return 0;
        }
    }
    if (kind == 0) {
        if (!jv_obj_get_last(c, "left") || !jv_obj_get_last(c, "right")) {
            *msg = "comparison condition requires 'left' and 'right'";
            return 0;
        }
    } else if (kind == 1) {
        JVal *conds = jv_obj_get_last(c, "conditions");
        if (!conds || conds->type != J_ARR) {
            *msg = "all/any requires a 'conditions' array";
            return 0;
        }
        for (i = 0; i < conds->u.arr.len; i++) {
            if (!validate_cond(conds->u.arr.items[i], msg))
                return 0;
        }
    } else {
        JVal *inner = jv_obj_get_last(c, "condition");
        if (!validate_cond(inner, msg))
            return 0;
    }
    return 1;
}

static int validate_retry(Engine *e, JVal *retry, Step *s)
{
    JVal *ma;
    size_t i;
    int64_t v;
    if (retry->type != J_OBJ) {
        set_err(e, "INVALID_WORKFLOW", s->id, "retry must be an object");
        return 0;
    }
    for (i = 0; i < retry->u.obj.len; i++) {
        if (!pair_key_is(&retry->u.obj.pairs[i], "max_attempts")) {
            set_err(e, "INVALID_WORKFLOW", s->id, "unknown field in retry");
            return 0;
        }
    }
    ma = jv_obj_get_last(retry, "max_attempts");
    if (!ma) {
        s->max_attempts = 1;
        return 1;
    }
    if (!int_of(ma, &v) || v < 1 || v > 5) {
        set_err(e, "INVALID_WORKFLOW", s->id,
                "retry.max_attempts must be an integer in 1..5");
        return 0;
    }
    s->max_attempts = (int)v;
    return 1;
}

static int validate_document(Engine *e, JVal *doc)
{
    JVal *version, *steps;
    size_t i, j;
    int *state;

    if (!doc || doc->type != J_OBJ) {
        set_err(e, "INVALID_WORKFLOW", NULL, "workflow document must be an object");
        return 0;
    }
    version = jv_obj_get_last(doc, "version");
    if (!version) {
        set_err(e, "INVALID_WORKFLOW", NULL, "missing workflow version");
        return 0;
    }
    {
        int64_t v;
        if (!int_of(version, &v) || v != 1) {
            set_err(e, "INVALID_WORKFLOW", NULL, "version must be the integer 1");
            return 0;
        }
    }
    steps = jv_obj_get_last(doc, "steps");
    if (!steps) {
        set_err(e, "INVALID_WORKFLOW", NULL, "missing workflow steps");
        return 0;
    }
    if (steps->type != J_ARR) {
        set_err(e, "INVALID_WORKFLOW", NULL, "steps must be an array");
        return 0;
    }
    for (i = 0; i < doc->u.obj.len; i++) {
        if (!pair_key_is(&doc->u.obj.pairs[i], "version") &&
            !pair_key_is(&doc->u.obj.pairs[i], "steps")) {
            set_err(e, "INVALID_WORKFLOW", NULL, "unknown top-level field");
            return 0;
        }
    }

    e->n = (int)steps->u.arr.len;
    e->steps = (Step *)arena_alloc(e->a, sizeof(Step) * (e->n ? (size_t)e->n : 1));
    memset(e->steps, 0, sizeof(Step) * (e->n ? (size_t)e->n : 1));

    /* Pass 1: ids, basic field shapes, ops, retry, when. */
    for (i = 0; i < (size_t)e->n; i++) {
        JVal *sv = steps->u.arr.items[i];
        Step *s = &e->steps[i];
        JVal *idv, *opv, *args, *deps, *when, *retry;
        const char *msg;

        if (!sv || sv->type != J_OBJ) {
            set_err(e, "INVALID_WORKFLOW", NULL, "step must be an object");
            return 0;
        }
        idv = jv_obj_get_last(sv, "id");
        if (!idv || idv->type != J_STR) {
            set_err(e, "INVALID_WORKFLOW", NULL, "step requires a string id");
            return 0;
        }
        s->id = arena_strndup(e->a, idv->u.str.s, idv->u.str.len);
        s->idlen = idv->u.str.len;
        if (!valid_ident(s->id, s->idlen)) {
            set_err(e, "INVALID_WORKFLOW", s->id, "invalid step id");
            return 0;
        }
        for (j = 0; j < i; j++) {
            if (e->steps[j].idlen == s->idlen && memcmp(e->steps[j].id, s->id, s->idlen) == 0) {
                set_err(e, "INVALID_WORKFLOW", s->id, "duplicate step id");
                return 0;
            }
        }

        opv = jv_obj_get_last(sv, "op");
        if (!opv || opv->type != J_STR) {
            set_err(e, "INVALID_WORKFLOW", s->id, "step requires a string op");
            return 0;
        }
        if (!known_op(opv)) {
            set_err(e, "INVALID_WORKFLOW", s->id, "unknown operation");
            return 0;
        }
        s->op = opv;

        for (j = 0; j < sv->u.obj.len; j++) {
            JPair *p = &sv->u.obj.pairs[j];
            if (!(pair_key_is(p, "id") || pair_key_is(p, "op") ||
                  pair_key_is(p, "args") || pair_key_is(p, "depends_on") ||
                  pair_key_is(p, "when") || pair_key_is(p, "retry"))) {
                set_err(e, "INVALID_WORKFLOW", s->id, "unknown step field");
                return 0;
            }
        }

        args = jv_obj_get_last(sv, "args");
        if (!args) {
            args = jv_obj(e->a);
        } else if (args->type != J_OBJ) {
            set_err(e, "INVALID_WORKFLOW", s->id, "args must be an object");
            return 0;
        }
        s->args = args;

        deps = jv_obj_get_last(sv, "depends_on");
        if (deps) {
            if (deps->type != J_ARR) {
                set_err(e, "INVALID_WORKFLOW", s->id, "depends_on must be an array");
                return 0;
            }
            s->ndeps = (int)deps->u.arr.len;
            s->deps = (int *)arena_alloc(e->a, sizeof(int) * (size_t)(s->ndeps ? s->ndeps : 1));
            for (j = 0; j < deps->u.arr.len; j++) {
                if (deps->u.arr.items[j]->type != J_STR) {
                    set_err(e, "INVALID_WORKFLOW", s->id,
                            "depends_on entries must be strings");
                    return 0;
                }
                s->deps[j] = -1;
            }
        }

        retry = jv_obj_get_last(sv, "retry");
        if (retry) {
            if (!validate_retry(e, retry, s))
                return 0;
        }
        s->max_attempts = s->max_attempts ? s->max_attempts : 1;

        when = jv_obj_get_last(sv, "when");
        if (when) {
            if (!validate_cond(when, &msg)) {
                set_err(e, "INVALID_WORKFLOW", s->id, msg);
                return 0;
            }
        }
        s->when = when;
    }

    /* Pass 2: dependencies exist. */
    for (i = 0; i < (size_t)e->n; i++) {
        JVal *deps = jv_obj_get_last(steps->u.arr.items[i], "depends_on");
        if (!deps)
            continue;
        for (j = 0; j < deps->u.arr.len; j++) {
            JVal *dv = deps->u.arr.items[j];
            int idx = find_step(e, dv->u.str.s, dv->u.str.len);
            if (idx < 0) {
                set_err(e, "UNKNOWN_DEPENDENCY", e->steps[i].id,
                        "depends_on names an unknown step");
                return 0;
            }
            e->steps[i].deps[j] = idx;
        }
    }

    /* Pass 3: cycles. */
    state = (int *)calloc((size_t)e->n ? (size_t)e->n : 1, sizeof(int));
    if (!state) {
        set_err(e, "INVALID_WORKFLOW", NULL, "out of memory");
        return 0;
    }
    {
        int cycle = 0;
        int k;
        /* iterative DFS to keep stack usage bounded */
        for (k = 0; k < e->n && !cycle; k++) {
            int *stack = (int *)malloc((size_t)e->n * sizeof(int) * 2 + 2);
            int *edge = (int *)malloc((size_t)e->n * sizeof(int) * 2 + 2);
            int sp = 0;
            if (!stack || !edge) {
                free(stack);
                free(edge);
                free(state);
                set_err(e, "INVALID_WORKFLOW", NULL, "out of memory");
                return 0;
            }
            if (state[k] == 0) {
                stack[sp] = k;
                edge[sp] = 0;
                sp++;
                state[k] = 1;
                while (sp > 0) {
                    int cur = stack[sp - 1];
                    Step *s = &e->steps[cur];
                    if (edge[sp - 1] < s->ndeps) {
                        int d = s->deps[edge[sp - 1]++];
                        if (state[d] == 1) {
                            cycle = 1;
                            break;
                        }
                        if (state[d] == 0) {
                            state[d] = 1;
                            stack[sp] = d;
                            edge[sp] = 0;
                            sp++;
                        }
                    } else {
                        state[cur] = 2;
                        sp--;
                    }
                }
            }
            free(stack);
            free(edge);
        }
        free(state);
        if (cycle) {
            set_err(e, "CYCLIC_DEPENDENCY", NULL, "dependency cycle detected");
            return 0;
        }
    }
    return 1;
}

/* ================================================================== */
/* Execution                                                           */
/* ================================================================== */

static int deps_done(Engine *e, Step *s)
{
    int i;
    for (i = 0; i < s->ndeps; i++) {
        if (!e->steps[s->deps[i]].done)
            return 0;
    }
    return 1;
}

static int execute(Engine *e)
{
    int finished = 0;

    while (finished < e->n) {
        int idx = -1;
        int i;
        Step *s;
        JVal *resolved, *result;
        OpErr err;

        for (i = 0; i < e->n; i++) {
            if (!e->steps[i].done && deps_done(e, &e->steps[i])) {
                idx = i;
                break;
            }
        }
        if (idx < 0) {
            set_err(e, "INTERNAL_ERROR", NULL, "no ready step (cycle?)");
            return 0;
        }
        s = &e->steps[idx];

        if (s->when) {
            int cond;
            err.code = NULL;
            if (!eval_cond(e, idx, s->when, &cond, &err)) {
                set_err(e, err.code, s->id, err.msg);
                return 0;
            }
            if (!cond) {
                s->done = 1;
                s->skipped = 1;
                s->result = jv_null(e->a);
                jv_obj_set(e->a, e->results, s->id, s->idlen, s->result);
                jv_arr_push(e->a, e->exec_order, jv_str(e->a, s->id, s->idlen));
                jv_arr_push(e->a, e->skipped_arr, jv_str(e->a, s->id, s->idlen));
                finished++;
                continue;
            }
        }

        err.code = NULL;
        if (!resolve_value(e, idx, s->args, &resolved, &err)) {
            set_err(e, err.code, s->id, err.msg);
            return 0;
        }

        result = NULL;
        {
            const char *final_code = NULL;
            char final_msg[256];
            int attempts = s->max_attempts > 0 ? s->max_attempts : 1;
            int a;
            final_msg[0] = '\0';
            for (a = 0; a < attempts; a++) {
                err.code = NULL;
                s->attempts++;
                if (run_op(e, s, resolved, &result, &err)) {
                    final_code = NULL;
                    break;
                }
                if (strcmp(err.code, "TRANSIENT_FAILURE") == 0) {
                    if (a + 1 < attempts)
                        continue;
                    final_code = "STEP_FAILED";
                    snprintf(final_msg, sizeof(final_msg),
                             "step failed after %d attempt(s)", attempts);
                    break;
                }
                final_code = err.code;
                snprintf(final_msg, sizeof(final_msg), "%s", err.msg);
                break;
            }
            if (final_code) {
                set_err(e, final_code, s->id, final_msg);
                return 0;
            }
        }

        s->done = 1;
        s->result = result;
        jv_obj_set(e->a, e->results, s->id, s->idlen, result);
        jv_arr_push(e->a, e->exec_order, jv_str(e->a, s->id, s->idlen));
        finished++;
    }
    return 1;
}

/* ================================================================== */
/* Response                                                            */
/* ================================================================== */

static int emit_response(Engine *e, int success, int exit_code)
{
    JVal *resp = jv_obj(e->a);
    JVal *error;
    Buf b;

    if (success) {
        error = jv_null(e->a);
    } else if (!e->has_err) {
        error = jv_null(e->a);
    } else {
        error = jv_obj(e->a);
        jv_obj_set(e->a, error, "code", 4,
                   jv_str(e->a, e->err.code, strlen(e->err.code)));
        if (e->err.step)
            jv_obj_set(e->a, error, "step", 4,
                       jv_str(e->a, e->err.step, strlen(e->err.step)));
        else
            jv_obj_set(e->a, error, "step", 4, jv_null(e->a));
        jv_obj_set(e->a, error, "message", 7, jv_str(e->a, e->err.msg, strlen(e->err.msg)));
    }

    jv_obj_set(e->a, resp, "status", 6,
               jv_str(e->a, success ? "completed" : "failed", success ? 9 : 6));
    jv_obj_set(e->a, resp, "results", 7, e->results);
    jv_obj_set(e->a, resp, "execution_order", 15, e->exec_order);
    jv_obj_set(e->a, resp, "skipped", 7, e->skipped_arr);
    jv_obj_set(e->a, resp, "error", 5, error);

    buf_init(&b);
    json_write(&b, resp);
    buf_putc(&b, '\n');
    if (b.len)
        fwrite(b.data, 1, b.len, stdout);
    fflush(stdout);
    buf_free(&b);
    return exit_code;
}

/* ================================================================== */
/* Input                                                               */
/* ================================================================== */

static char *read_stream(FILE *f, size_t *out_len)
{
    Buf b;
    char chunk[8192];
    size_t n;
    buf_init(&b);
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
        buf_append(&b, chunk, n);
    if (ferror(f)) {
        buf_free(&b);
        return NULL;
    }
    buf_putc(&b, '\0');
    *out_len = b.len - 1;
    return b.data;
}

int main(int argc, char **argv)
{
    Arena arena;
    Engine e;
    char *input = NULL;
    size_t input_len = 0;
    JVal *doc;
    const char *parse_err = NULL;
    int ok;
    int exit_code;

    arena_init(&arena);
    memset(&e, 0, sizeof(e));
    e.a = &arena;
    e.results = jv_obj(&arena);
    e.exec_order = jv_arr(&arena);
    e.skipped_arr = jv_arr(&arena);

    if (argc != 3 || strcmp(argv[1], "run") != 0) {
        set_err(&e, "INVALID_WORKFLOW", NULL, "usage: workflow run <workflow.json|->");
        return emit_response(&e, 0, 2);
    }

    if (strcmp(argv[2], "-") == 0) {
        input = read_stream(stdin, &input_len);
    } else {
        FILE *f = fopen(argv[2], "rb");
        if (!f) {
            set_err(&e, "INVALID_WORKFLOW", NULL, "cannot open input document");
            return emit_response(&e, 0, 2);
        }
        input = read_stream(f, &input_len);
        fclose(f);
    }
    if (!input) {
        set_err(&e, "INVALID_WORKFLOW", NULL, "cannot read input document");
        return emit_response(&e, 0, 2);
    }

    doc = json_parse(&arena, input, input_len, &parse_err);
    free(input);
    if (!doc) {
        set_err(&e, "INVALID_WORKFLOW", NULL, parse_err ? parse_err : "invalid JSON");
        return emit_response(&e, 0, 2);
    }

    ok = validate_document(&e, doc);
    if (!ok) {
        int code = 2;
        if (e.err.code && strcmp(e.err.code, "INTERNAL_ERROR") == 0)
            code = 3;
        return emit_response(&e, 0, code);
    }

    ok = execute(&e);
    if (!ok) {
        int code = 1;
        if (e.err.code && strcmp(e.err.code, "INTERNAL_ERROR") == 0)
            code = 3;
        exit_code = code;
        return emit_response(&e, 0, exit_code);
    }

    return emit_response(&e, 1, 0);
}
