#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <limits.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* errors                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *code;
    const char *step;
    char msg[512];
} WErr;

static bool err_set(WErr *e, const char *code, const char *fmt, ...) {
    e->code = code;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->msg, sizeof(e->msg), fmt, ap);
    va_end(ap);
    return false;
}

static bool vfail(WErr *e, const char *code, const char *step, const char *fmt, ...) {
    e->code = code;
    e->step = step;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->msg, sizeof(e->msg), fmt, ap);
    va_end(ap);
    return false;
}

/* ------------------------------------------------------------------ */
/* operations                                                          */
/* ------------------------------------------------------------------ */

typedef enum {
    OP_ADD, OP_SUBTRACT, OP_MULTIPLY, OP_DIVIDE, OP_MOD,
    OP_SUM, OP_MIN, OP_MAX, OP_RANGE,
    OP_CONCAT, OP_SPLIT, OP_JOIN, OP_UPPERCASE, OP_LOWERCASE, OP_TRIM,
    OP_REPLACE, OP_LENGTH,
    OP_MAP, OP_FILTER, OP_REDUCE, OP_SORT, OP_REVERSE, OP_UNIQUE,
    OP_SLICE, OP_FLATTEN,
    OP_GET, OP_SET, OP_KEYS, OP_VALUES, OP_MERGE, OP_PICK, OP_OMIT,
    OP_CONSTANT, OP_EQUALS, OP_COMPARE, OP_AND, OP_OR, OP_NOT, OP_TYPE,
    OP_FAIL_UNTIL
} OpKind;

typedef struct {
    const char *name;
    OpKind kind;
    const char *args[4];
    int nargs;
} OpDef;

static const OpDef OPS[] = {
    {"add", OP_ADD, {"a", "b"}, 2},
    {"subtract", OP_SUBTRACT, {"a", "b"}, 2},
    {"multiply", OP_MULTIPLY, {"a", "b"}, 2},
    {"divide", OP_DIVIDE, {"a", "b"}, 2},
    {"mod", OP_MOD, {"a", "b"}, 2},
    {"sum", OP_SUM, {"input"}, 1},
    {"min", OP_MIN, {"input"}, 1},
    {"max", OP_MAX, {"input"}, 1},
    {"range", OP_RANGE, {"start", "end"}, 2},
    {"concat", OP_CONCAT, {"a", "b"}, 2},
    {"split", OP_SPLIT, {"input", "sep"}, 2},
    {"join", OP_JOIN, {"input", "sep"}, 2},
    {"uppercase", OP_UPPERCASE, {"input"}, 1},
    {"lowercase", OP_LOWERCASE, {"input"}, 1},
    {"trim", OP_TRIM, {"input"}, 1},
    {"replace", OP_REPLACE, {"input", "search", "replace"}, 3},
    {"length", OP_LENGTH, {"input"}, 1},
    {"map", OP_MAP, {"input", "transform"}, 2},
    {"filter", OP_FILTER, {"input", "predicate"}, 2},
    {"reduce", OP_REDUCE, {"input", "op", "initial"}, 3},
    {"sort", OP_SORT, {"input"}, 1},
    {"reverse", OP_REVERSE, {"input"}, 1},
    {"unique", OP_UNIQUE, {"input"}, 1},
    {"slice", OP_SLICE, {"input", "start", "end"}, 3},
    {"flatten", OP_FLATTEN, {"input"}, 1},
    {"get", OP_GET, {"input", "key"}, 2},
    {"set", OP_SET, {"input", "key", "value"}, 3},
    {"keys", OP_KEYS, {"input"}, 1},
    {"values", OP_VALUES, {"input"}, 1},
    {"merge", OP_MERGE, {"a", "b"}, 2},
    {"pick", OP_PICK, {"input", "keys"}, 2},
    {"omit", OP_OMIT, {"input", "keys"}, 2},
    {"constant", OP_CONSTANT, {"value"}, 1},
    {"equals", OP_EQUALS, {"a", "b"}, 2},
    {"compare", OP_COMPARE, {"a", "b", "op"}, 3},
    {"and", OP_AND, {"a", "b"}, 2},
    {"or", OP_OR, {"a", "b"}, 2},
    {"not", OP_NOT, {"a"}, 1},
    {"type", OP_TYPE, {"value"}, 1},
    {"fail_until", OP_FAIL_UNTIL, {"failures", "value"}, 2},
};

static const size_t NOPS = sizeof(OPS) / sizeof(OPS[0]);

static const OpDef *find_op(const char *name) {
    for (size_t i = 0; i < NOPS; i++)
        if (strcmp(OPS[i].name, name) == 0) return &OPS[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* engine structures                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    char *id;
    const OpDef *def;
    OpKind kind;
    JVal *args;
    JVal *when;            /* NULL when absent */
    int max_attempts;
    /* dependency info */
    char **depnames;
    size_t *deplens;
    size_t *deps;
    size_t ndeps;
} Step;

typedef struct {
    Step *steps;
    size_t n;
    bool *reach;          /* n*n transitive dependency matrix */
    bool *done;
    bool *skipped;
    JVal **values;
    int *attempts;
    JVal *results_obj;
    JVal *order;
    JVal *skipped_arr;
} Engine;

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    char *p;
    size_t len, cap;
} SB;

static void sb_init(SB *b) { b->p = NULL; b->len = 0; b->cap = 0; }

static void sb_grow(SB *b, size_t extra) {
    if (b->len + extra + 1 > b->cap) {
        while (b->len + extra + 1 > b->cap) b->cap = b->cap ? b->cap * 2 : 32;
        b->p = xrealloc(b->p, b->cap);
    }
}

static void sb_putn(SB *b, const char *s, size_t n) {
    sb_grow(b, n);
    if (n) memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void sb_putc(SB *b, char c) { sb_putn(b, &c, 1); }

static char *sb_finish(SB *b) {
    sb_grow(b, 0);
    b->p[b->len] = '\0';
    return b->p;
}

static bool valid_id(const char *s, size_t n) {
    if (n == 0) return false;
    unsigned char c = (unsigned char)s[0];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'))
        return false;
    for (size_t i = 1; i < n; i++) {
        c = (unsigned char)s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_'))
            return false;
    }
    return true;
}

static bool is_ascii_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static bool is_self_obj(const JVal *v) {
    return v->t == J_OBJ && v->olen == 1 && v->klens[0] == 5 &&
           memcmp(v->keys[0], "$self", 5) == 0;
}

static bool is_ref_obj(const JVal *v) {
    return v->t == J_OBJ && v->olen == 1 && v->klens[0] == 4 &&
           memcmp(v->keys[0], "$ref", 4) == 0 && v->vals[0]->t == J_STR;
}

static size_t *sorted_indices(const JVal *o) {
    size_t n = o->olen;
    size_t *idx = xmalloc((n ? n : 1) * sizeof(size_t));
    for (size_t i = 0; i < n; i++) idx[i] = i;
    for (size_t i = 1; i < n; i++) {
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

/* stable merge sort over arrays of JVal* */
static void msort(JVal **a, JVal **tmp, size_t n) {
    if (n < 2) return;
    size_t m = n / 2;
    msort(a, tmp, m);
    msort(a + m, tmp, n - m);
    size_t i = 0, j = m, k = 0;
    while (i < m && j < n) {
        if (jcmp(a[j], a[i]) < 0) tmp[k++] = a[j++];
        else tmp[k++] = a[i++];
    }
    while (i < m) tmp[k++] = a[i++];
    while (j < n) tmp[k++] = a[j++];
    memcpy(a, tmp, n * sizeof(JVal *));
}

/* ------------------------------------------------------------------ */
/* argument helpers                                                    */
/* ------------------------------------------------------------------ */

static JVal *arg_any(JVal *args, const char *name, WErr *e) {
    JVal *v = jobj_get(args, name);
    if (!v) err_set(e, "INVALID_ARG", "missing required argument '%s'", name);
    return v;
}

static bool get_i64(JVal *v, const char *name, int64_t *out, WErr *e) {
    if (v->t != J_NUM || !v->is_int) {
        return err_set(e, "INVALID_ARG", "argument '%s' must be a 64-bit integer literal", name);
    }
    *out = v->i;
    return true;
}

static bool arg_i64(JVal *args, const char *name, int64_t *out, WErr *e) {
    JVal *v = arg_any(args, name, e);
    if (!v) return false;
    return get_i64(v, name, out, e);
}

static bool arg_str(JVal *args, const char *name, const char **s, size_t *n, WErr *e) {
    JVal *v = arg_any(args, name, e);
    if (!v) return false;
    if (v->t != J_STR) return err_set(e, "INVALID_ARG", "argument '%s' must be a string", name);
    *s = v->s;
    *n = v->slen;
    return true;
}

static bool arg_arr(JVal *args, const char *name, JVal **out, WErr *e) {
    JVal *v = arg_any(args, name, e);
    if (!v) return false;
    if (v->t != J_ARR) return err_set(e, "INVALID_ARG", "argument '%s' must be an array", name);
    *out = v;
    return true;
}

static bool arg_obj(JVal *args, const char *name, JVal **out, WErr *e) {
    JVal *v = arg_any(args, name, e);
    if (!v) return false;
    if (v->t != J_OBJ) return err_set(e, "INVALID_ARG", "argument '%s' must be an object", name);
    *out = v;
    return true;
}

static bool check_args(JVal *args, const OpDef *def, WErr *e) {
    for (size_t i = 0; i < args->olen; i++) {
        bool found = false;
        for (int j = 0; j < def->nargs; j++) {
            const char *a = def->args[j];
            if (args->klens[i] == strlen(a) && memcmp(args->keys[i], a, args->klens[i]) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            return err_set(e, "INVALID_ARG", "unknown argument '%.*s' for op '%s'",
                           (int)args->klens[i], args->keys[i], def->name);
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* arithmetic                                                          */
/* ------------------------------------------------------------------ */

static bool apply_bin(OpKind k, int64_t a, int64_t b, int64_t *r, WErr *e) {
    switch (k) {
    case OP_ADD:
        if (!__builtin_add_overflow(a, b, r)) return true;
        break;
    case OP_SUBTRACT:
        if (!__builtin_sub_overflow(a, b, r)) return true;
        break;
    case OP_MULTIPLY:
        if (!__builtin_mul_overflow(a, b, r)) return true;
        break;
    case OP_DIVIDE:
        if (b == 0) return err_set(e, "DIVISION_BY_ZERO", "division by zero");
        if (a == INT64_MIN && b == -1)
            return err_set(e, "ARITHMETIC_OVERFLOW", "INT64_MIN / -1 overflows");
        *r = a / b;
        return true;
    case OP_MOD:
        if (b == 0) return err_set(e, "DIVISION_BY_ZERO", "modulo by zero");
        if (a == INT64_MIN && b == -1)
            return err_set(e, "ARITHMETIC_OVERFLOW", "INT64_MIN %% -1 overflows");
        *r = a % b;
        return true;
    default:
        return err_set(e, "INVALID_ARG", "not a binary arithmetic operator");
    }
    return err_set(e, "ARITHMETIC_OVERFLOW", "arithmetic overflow");
}

/* ------------------------------------------------------------------ */
/* references                                                          */
/* ------------------------------------------------------------------ */

static bool parse_arr_index(const char *s, size_t n, size_t *out) {
    if (n == 0) return false;
    size_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        size_t d = (size_t)(s[i] - '0');
        if (v > (SIZE_MAX - d) / 10) return false;
        v = v * 10 + d;
    }
    *out = v;
    return true;
}

static JVal *resolve_value(Engine *en, size_t cur, JVal *v, WErr *e);

static JVal *resolve_ref(Engine *en, size_t cur, const char *path, size_t plen, WErr *e) {
    const char *dot = memchr(path, '.', plen);
    size_t idlen = dot ? (size_t)(dot - path) : plen;
    size_t idx = SIZE_MAX;
    for (size_t i = 0; i < en->n; i++) {
        if (strlen(en->steps[i].id) == idlen &&
            memcmp(en->steps[i].id, path, idlen) == 0) {
            idx = i;
            break;
        }
    }
    if (idx == SIZE_MAX) {
        err_set(e, "INVALID_REFERENCE", "unknown step '%.*s' in reference", (int)idlen, path);
        return NULL;
    }
    if (idx == cur || !en->reach[cur * en->n + idx]) {
        err_set(e, "INVALID_REFERENCE", "step '%.*s' is not a dependency of the referencing step",
                (int)idlen, path);
        return NULL;
    }
    if (en->skipped[idx]) {
        err_set(e, "SKIPPED_REFERENCE", "step '%.*s' was skipped", (int)idlen, path);
        return NULL;
    }
    if (!en->done[idx]) {
        err_set(e, "INVALID_REFERENCE", "step '%.*s' has not finished", (int)idlen, path);
        return NULL;
    }
    JVal *curv = en->values[idx];
    const char *p = path + idlen;
    const char *pend = path + plen;
    while (p < pend) {
        p++; /* skip '.' */
        const char *seg = p;
        while (p < pend && *p != '.') p++;
        size_t seglen = (size_t)(p - seg);
        if (curv->t == J_OBJ) {
            JVal *nv = jobj_getn(curv, seg, seglen);
            if (!nv) {
                err_set(e, "INVALID_REFERENCE", "path segment '%.*s' not found", (int)seglen, seg);
                return NULL;
            }
            curv = nv;
        } else if (curv->t == J_ARR) {
            size_t ix = 0;
            if (!parse_arr_index(seg, seglen, &ix)) {
                err_set(e, "INVALID_REFERENCE", "invalid array index '%.*s'", (int)seglen, seg);
                return NULL;
            }
            if (ix >= curv->alen) {
                err_set(e, "INVALID_REFERENCE", "array index %zu out of bounds", ix);
                return NULL;
            }
            curv = curv->items[ix];
        } else {
            err_set(e, "INVALID_REFERENCE", "cannot descend into a %s value", jtype_name(curv));
            return NULL;
        }
    }
    return curv;
}

static JVal *resolve_value(Engine *en, size_t cur, JVal *v, WErr *e) {
    if (is_ref_obj(v)) {
        return resolve_ref(en, cur, v->vals[0]->s, v->vals[0]->slen, e);
    }
    switch (v->t) {
    case J_ARR: {
        JVal *out = jarr();
        for (size_t i = 0; i < v->alen; i++) {
            JVal *r = resolve_value(en, cur, v->items[i], e);
            if (!r) return NULL;
            jarr_push(out, r);
        }
        return out;
    }
    case J_OBJ: {
        JVal *out = jobj();
        for (size_t i = 0; i < v->olen; i++) {
            JVal *r = resolve_value(en, cur, v->vals[i], e);
            if (!r) return NULL;
            jobj_put(out, v->keys[i], v->klens[i], r);
        }
        return out;
    }
    default:
        return v;
    }
}

/* ------------------------------------------------------------------ */
/* conditions                                                          */
/* ------------------------------------------------------------------ */

typedef enum {
    CMP_EQ, CMP_NE, CMP_GT, CMP_GTE, CMP_LT, CMP_LTE
} CmpKind;

static bool cmp_eval(CmpKind c, const JVal *a, const JVal *b) {
    switch (c) {
    case CMP_EQ: return jeq(a, b);
    case CMP_NE: return !jeq(a, b);
    case CMP_GT: return jcmp(a, b) > 0;
    case CMP_GTE: return jcmp(a, b) >= 0;
    case CMP_LT: return jcmp(a, b) < 0;
    case CMP_LTE: return jcmp(a, b) <= 0;
    }
    return false;
}

static bool cmp_from_name(const char *op, CmpKind *out) {
    if (strcmp(op, "eq") == 0) { *out = CMP_EQ; return true; }
    if (strcmp(op, "ne") == 0) { *out = CMP_NE; return true; }
    if (strcmp(op, "gt") == 0) { *out = CMP_GT; return true; }
    if (strcmp(op, "gte") == 0) { *out = CMP_GTE; return true; }
    if (strcmp(op, "lt") == 0) { *out = CMP_LT; return true; }
    if (strcmp(op, "lte") == 0) { *out = CMP_LTE; return true; }
    return false;
}

static bool eval_cond(Engine *en, size_t cur, JVal *w, bool *out, WErr *e) {
    if (is_ref_obj(w)) {
        JVal *r = resolve_value(en, cur, w, e);
        if (!r) return false;
        if (r->t != J_BOOL) {
            return err_set(e, "INVALID_CONDITION", "condition reference did not yield a boolean");
        }
        *out = r->b;
        return true;
    }
    if (w->t == J_BOOL) {
        *out = w->b;
        return true;
    }
    if (w->t != J_OBJ) {
        return err_set(e, "INVALID_CONDITION", "condition is not a boolean");
    }
    JVal *opv = jobj_get(w, "op");
    if (!opv || opv->t != J_STR) {
        return err_set(e, "INVALID_CONDITION", "condition is missing an 'op'");
    }
    const char *op = opv->s;
    CmpKind c;
    if (cmp_from_name(op, &c)) {
        JVal *lv = jobj_get(w, "left");
        JVal *rv = jobj_get(w, "right");
        JVal *l = resolve_value(en, cur, lv, e);
        if (!l) return false;
        JVal *r = resolve_value(en, cur, rv, e);
        if (!r) return false;
        *out = cmp_eval(c, l, r);
        return true;
    }
    if (strcmp(op, "all") == 0 || strcmp(op, "any") == 0) {
        JVal *conds = jobj_get(w, "conditions");
        bool is_all = op[0] == 'a' && op[1] == 'l';
        for (size_t i = 0; i < conds->alen; i++) {
            bool v = false;
            if (!eval_cond(en, cur, conds->items[i], &v, e)) return false;
            if (is_all && !v) { *out = false; return true; }
            if (!is_all && v) { *out = true; return true; }
        }
        *out = is_all;
        return true;
    }
    if (strcmp(op, "not") == 0) {
        JVal *cnd = jobj_get(w, "condition");
        bool v = false;
        if (!eval_cond(en, cur, cnd, &v, e)) return false;
        *out = !v;
        return true;
    }
    return err_set(e, "INVALID_CONDITION", "unknown condition operator '%s'", op);
}

/* ------------------------------------------------------------------ */
/* map transforms                                                      */
/* ------------------------------------------------------------------ */

typedef enum {
    TR_CONST, TR_BIN, TR_NEGATE, TR_ABS, TR_STRINGIFY, TR_TYPE
} TrKind;

typedef struct {
    TrKind kind;
    OpKind bin;
    JVal *cval;
    int a_self, b_self;
    JVal *a_const, *b_const;
    int64_t a_int, b_int;
} Transform;

static bool tr_has_only(JVal *t, const char **allowed, int n, WErr *e) {
    for (size_t i = 0; i < t->olen; i++) {
        bool ok = false;
        for (int j = 0; j < n; j++) {
            if (t->klens[i] == strlen(allowed[j]) &&
                memcmp(t->keys[i], allowed[j], t->klens[i]) == 0) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            return err_set(e, "INVALID_ARG", "unknown transform field '%.*s'",
                           (int)t->klens[i], t->keys[i]);
        }
    }
    return true;
}

static bool parse_transform(JVal *t, Transform *tr, WErr *e) {
    memset(tr, 0, sizeof(*tr));
    const char *type = NULL;
    if (t->t == J_STR) {
        /* compatibility: a bare transform name such as "negate" */
        type = t->s;
    } else if (t->t == J_OBJ) {
        JVal *tv = jobj_get(t, "type");
        if (!tv || tv->t != J_STR)
            return err_set(e, "INVALID_ARG", "transform requires a string 'type'");
        type = tv->s;
    } else {
        return err_set(e, "INVALID_ARG", "transform must be an object");
    }
    if (t->t != J_OBJ) {
        if (strcmp(type, "negate") == 0) { tr->kind = TR_NEGATE; return true; }
        if (strcmp(type, "abs") == 0) { tr->kind = TR_ABS; return true; }
        if (strcmp(type, "stringify") == 0) { tr->kind = TR_STRINGIFY; return true; }
        if (strcmp(type, "type") == 0) { tr->kind = TR_TYPE; return true; }
        return err_set(e, "INVALID_ARG", "transform '%s' requires an object form", type);
    }

    if (strcmp(type, "constant") == 0) {
        const char *allowed[] = {"type", "value"};
        if (!tr_has_only(t, allowed, 2, e)) return false;
        JVal *val = arg_any(t, "value", e);
        if (!val) return false;
        tr->kind = TR_CONST;
        tr->cval = val;
        return true;
    }
    if (strcmp(type, "negate") == 0) {
        const char *allowed[] = {"type"};
        if (!tr_has_only(t, allowed, 1, e)) return false;
        tr->kind = TR_NEGATE;
        return true;
    }
    if (strcmp(type, "abs") == 0) {
        const char *allowed[] = {"type"};
        if (!tr_has_only(t, allowed, 1, e)) return false;
        tr->kind = TR_ABS;
        return true;
    }
    if (strcmp(type, "stringify") == 0) {
        const char *allowed[] = {"type"};
        if (!tr_has_only(t, allowed, 1, e)) return false;
        tr->kind = TR_STRINGIFY;
        return true;
    }
    if (strcmp(type, "type") == 0) {
        const char *allowed[] = {"type"};
        if (!tr_has_only(t, allowed, 1, e)) return false;
        tr->kind = TR_TYPE;
        return true;
    }
    OpKind bin;
    if (strcmp(type, "add") == 0) bin = OP_ADD;
    else if (strcmp(type, "subtract") == 0) bin = OP_SUBTRACT;
    else if (strcmp(type, "multiply") == 0) bin = OP_MULTIPLY;
    else if (strcmp(type, "divide") == 0) bin = OP_DIVIDE;
    else if (strcmp(type, "mod") == 0) bin = OP_MOD;
    else if (strcmp(type, "op") == 0) {
        const char *allowed[] = {"type", "op", "args"};
        if (!tr_has_only(t, allowed, 3, e)) return false;
        JVal *opv = arg_any(t, "op", e);
        if (!opv) return false;
        if (opv->t != J_STR) return err_set(e, "INVALID_ARG", "transform op must be a string");
        if (strcmp(opv->s, "add") == 0) bin = OP_ADD;
        else if (strcmp(opv->s, "subtract") == 0) bin = OP_SUBTRACT;
        else if (strcmp(opv->s, "multiply") == 0) bin = OP_MULTIPLY;
        else if (strcmp(opv->s, "divide") == 0) bin = OP_DIVIDE;
        else if (strcmp(opv->s, "mod") == 0) bin = OP_MOD;
        else return err_set(e, "INVALID_ARG", "unsupported transform op '%s'", opv->s);
        JVal *args = jobj_get(t, "args");
        if (!args) return err_set(e, "INVALID_ARG", "transform requires 'args'");
        if (args->t != J_OBJ) return err_set(e, "INVALID_ARG", "transform args must be an object");
        for (size_t i = 0; i < args->olen; i++) {
            bool is_a = args->klens[i] == 1 && args->keys[i][0] == 'a';
            bool is_b = args->klens[i] == 1 && args->keys[i][0] == 'b';
            if (!is_a && !is_b)
                return err_set(e, "INVALID_ARG", "unknown transform arg '%.*s'",
                               (int)args->klens[i], args->keys[i]);
        }
        JVal *av = jobj_get(args, "a");
        JVal *bv = jobj_get(args, "b");
        if (!av || !bv) return err_set(e, "INVALID_ARG", "transform op requires args 'a' and 'b'");
        tr->kind = TR_BIN;
        tr->bin = bin;
        if (is_self_obj(av)) tr->a_self = 1;
        else {
            if (!get_i64(av, "a", &tr->a_int, e)) return false;
        }
        if (is_self_obj(bv)) tr->b_self = 1;
        else {
            if (!get_i64(bv, "b", &tr->b_int, e)) return false;
        }
        return true;
    } else {
        return err_set(e, "INVALID_ARG", "unknown transform type '%s'", type);
    }

    const char *allowed[] = {"type", "value"};
    if (!tr_has_only(t, allowed, 2, e)) return false;
    JVal *val = arg_any(t, "value", e);
    if (!val) return false;
    int64_t n = 0;
    if (!get_i64(val, "value", &n, e)) return false;
    tr->kind = TR_BIN;
    tr->bin = bin;
    tr->a_self = 1;
    tr->b_self = 0;
    tr->b_int = n;
    return true;
}

static bool elem_int(JVal *el, int64_t *out, WErr *e) {
    if (el->t != J_NUM || !el->is_int)
        return err_set(e, "INVALID_ARG", "map element must be a 64-bit integer");
    *out = el->i;
    return true;
}

static bool tr_apply(Transform *tr, JVal *el, JVal **out, WErr *e) {
    switch (tr->kind) {
    case TR_CONST:
        *out = tr->cval;
        return true;
    case TR_STRINGIFY: {
        size_t n = 0;
        char *s = jwrite_alloc(el, &n);
        *out = jstrn(s, n);
        return true;
    }
    case TR_TYPE:
        *out = jstr(jtype_name(el));
        return true;
    case TR_NEGATE: {
        int64_t v = 0;
        if (!elem_int(el, &v, e)) return false;
        if (v == INT64_MIN) return err_set(e, "ARITHMETIC_OVERFLOW", "negate overflow");
        *out = jint(-v);
        return true;
    }
    case TR_ABS: {
        int64_t v = 0;
        if (!elem_int(el, &v, e)) return false;
        if (v == INT64_MIN) return err_set(e, "ARITHMETIC_OVERFLOW", "abs overflow");
        *out = jint(v < 0 ? -v : v);
        return true;
    }
    case TR_BIN: {
        int64_t a = 0, b = 0, r = 0;
        if (tr->a_self) {
            if (!elem_int(el, &a, e)) return false;
        } else a = tr->a_int;
        if (tr->b_self) {
            if (!elem_int(el, &b, e)) return false;
        } else b = tr->b_int;
        if (!apply_bin(tr->bin, a, b, &r, e)) return false;
        *out = jint(r);
        return true;
    }
    }
    return err_set(e, "INVALID_ARG", "bad transform");
}

/* ------------------------------------------------------------------ */
/* filter predicates                                                   */
/* ------------------------------------------------------------------ */

typedef enum {
    PR_EVEN, PR_ODD, PR_CMP, PR_TRUTHY, PR_FALSY, PR_TYPE_IS, PR_CONTAINS
} PrKind;

typedef struct {
    PrKind kind;
    CmpKind cmp;
    JVal *val;
    int64_t n;
} Predicate;

static bool pred_only_fields(JVal *p, const char **fields, int n, WErr *e) {
    for (size_t i = 0; i < p->olen; i++) {
        bool ok = false;
        for (int j = 0; j < n; j++)
            if (p->klens[i] == strlen(fields[j]) &&
                memcmp(p->keys[i], fields[j], p->klens[i]) == 0)
                ok = true;
        if (!ok)
            return err_set(e, "INVALID_ARG", "unknown predicate field '%.*s'",
                           (int)p->klens[i], p->keys[i]);
    }
    return true;
}

static bool parse_predicate(JVal *p, Predicate *pr, WErr *e) {
    memset(pr, 0, sizeof(*pr));
    const char *type = NULL;
    if (p->t == J_STR) {
        /* compatibility: a bare predicate name such as "even" */
        type = p->s;
    } else if (p->t == J_OBJ) {
        JVal *tv = jobj_get(p, "type");
        if (!tv || tv->t != J_STR)
            return err_set(e, "INVALID_ARG", "predicate requires a string 'type'");
        type = tv->s;
    } else {
        return err_set(e, "INVALID_ARG", "predicate must be an object");
    }

    static const char *only_type[] = {"type"};
    if (strcmp(type, "even") == 0) {
        pr->kind = PR_EVEN;
        return p->t == J_OBJ ? pred_only_fields(p, only_type, 1, e) : true;
    }
    if (strcmp(type, "odd") == 0) {
        pr->kind = PR_ODD;
        return p->t == J_OBJ ? pred_only_fields(p, only_type, 1, e) : true;
    }
    if (strcmp(type, "truthy") == 0) {
        pr->kind = PR_TRUTHY;
        return p->t == J_OBJ ? pred_only_fields(p, only_type, 1, e) : true;
    }
    if (strcmp(type, "falsy") == 0) {
        pr->kind = PR_FALSY;
        return p->t == J_OBJ ? pred_only_fields(p, only_type, 1, e) : true;
    }
    if (p->t != J_OBJ)
        return err_set(e, "INVALID_ARG", "predicate '%s' requires an object form", type);

    CmpKind c;
    if (cmp_from_name(type, &c)) {
        static const char *fs[] = {"type", "value"};
        if (!pred_only_fields(p, fs, 2, e)) return false;
        pr->kind = PR_CMP;
        pr->cmp = c;
        int64_t n = 0;
        if (!arg_i64(p, "value", &n, e)) return false;
        pr->n = n;
        pr->val = jint(n);
        return true;
    }
    if (strcmp(type, "type_is") == 0) {
        pr->kind = PR_TYPE_IS;
        /* The spec writes this predicate as {"type": "type_is", "type": "<type>"},
         * which cannot be represented as JSON; accept the natural field names. */
        static const char *candidates[] = {"value", "type_name", "expected",
                                           "target", "name"};
        const char *s = NULL;
        size_t n = 0;
        for (int ci = 0; ci < 5; ci++) {
            JVal *v = jobj_get(p, candidates[ci]);
            if (v) {
                if (v->t != J_STR)
                    return err_set(e, "INVALID_ARG", "type_is target must be a string");
                s = v->s;
                n = v->slen;
                break;
            }
        }
        if (!s) return err_set(e, "INVALID_ARG", "type_is requires the target type name");
        for (size_t i = 0; i < p->olen; i++) {
            if (p->klens[i] == 4 && memcmp(p->keys[i], "type", 4) == 0) continue;
            bool ok = false;
            for (int ci = 0; ci < 5; ci++)
                if (p->klens[i] == strlen(candidates[ci]) &&
                    memcmp(p->keys[i], candidates[ci], p->klens[i]) == 0)
                    ok = true;
            if (!ok)
                return err_set(e, "INVALID_ARG", "unknown predicate field '%.*s'",
                               (int)p->klens[i], p->keys[i]);
        }
        pr->val = jstrn(s, n);
        return true;
    }
    if (strcmp(type, "string_contains") == 0) {
        static const char *fs[] = {"type", "value"};
        if (!pred_only_fields(p, fs, 2, e)) return false;
        pr->kind = PR_CONTAINS;
        const char *s;
        size_t n;
        if (!arg_str(p, "value", &s, &n, e)) return false;
        pr->val = jstrn(s, n);
        return true;
    }
    return err_set(e, "INVALID_ARG", "unknown predicate type '%s'", type);
}

static bool pred_apply(Predicate *pr, JVal *el, bool *out, WErr *e) {
    switch (pr->kind) {
    case PR_EVEN:
    case PR_ODD: {
        int64_t v = 0;
        if (!elem_int(el, &v, e)) return false;
        bool even = (v % 2) == 0;
        *out = pr->kind == PR_EVEN ? even : !even;
        return true;
    }
    case PR_CMP:
        *out = cmp_eval(pr->cmp, el, pr->val);
        return true;
    case PR_TRUTHY:
        *out = jtruthy(el);
        return true;
    case PR_FALSY:
        *out = !jtruthy(el);
        return true;
    case PR_TYPE_IS:
        *out = strcmp(jtype_name(el), pr->val->s) == 0;
        return true;
    case PR_CONTAINS: {
        if (el->t != J_STR)
            return err_set(e, "INVALID_ARG", "string_contains requires string elements");
        const char *hay = el->s;
        size_t hn = el->slen;
        const char *needle = pr->val->s;
        size_t nn = pr->val->slen;
        bool found = false;
        if (nn == 0) found = true;
        else if (nn <= hn) {
            for (size_t i = 0; i + nn <= hn; i++) {
                if (memcmp(hay + i, needle, nn) == 0) { found = true; break; }
            }
        }
        *out = found;
        return true;
    }
    }
    return err_set(e, "INVALID_ARG", "bad predicate");
}

/* ------------------------------------------------------------------ */
/* operation execution                                                 */
/* ------------------------------------------------------------------ */

static bool split_string(const char *s, size_t n, const char *sep, size_t sepn, JVal *out) {
    if (sepn == 0) {
        size_t i = 0;
        while (i < n) {
            size_t start = i;
            i++;
            while (i < n && (((unsigned char)s[i]) & 0xC0) == 0x80) i++;
            jarr_push(out, jstrn(s + start, i - start));
        }
        return true;
    }
    size_t i = 0;
    while (i + sepn <= n) {
        if (memcmp(s + i, sep, sepn) == 0) {
            jarr_push(out, jstrn(s, i));
            s += i + sepn;
            n -= i + sepn;
            i = 0;
        } else {
            i++;
        }
    }
    jarr_push(out, jstrn(s, n));
    return true;
}

static JVal *exec_op(Engine *en, size_t si, JVal *args, int attempt, WErr *e) {
    Step *st = &en->steps[si];
    OpKind k = st->kind;
    if (!check_args(args, st->def, e)) return NULL;

    switch (k) {
    case OP_ADD:
    case OP_SUBTRACT:
    case OP_MULTIPLY:
    case OP_DIVIDE:
    case OP_MOD: {
        int64_t a = 0, b = 0, r = 0;
        if (!arg_i64(args, "a", &a, e)) return NULL;
        if (!arg_i64(args, "b", &b, e)) return NULL;
        if (!apply_bin(k, a, b, &r, e)) return NULL;
        return jint(r);
    }
    case OP_SUM: {
        JVal *in;
        if (!arg_arr(args, "input", &in, e)) return NULL;
        int64_t acc = 0;
        for (size_t i = 0; i < in->alen; i++) {
            int64_t v = 0;
            if (!get_i64(in->items[i], "input element", &v, e)) return NULL;
            if (__builtin_add_overflow(acc, v, &acc))
                return err_set(e, "ARITHMETIC_OVERFLOW", "overflow in sum"), NULL;
        }
        return jint(acc);
    }
    case OP_MIN:
    case OP_MAX: {
        JVal *in;
        if (!arg_arr(args, "input", &in, e)) return NULL;
        if (in->alen == 0) return err_set(e, "EMPTY_ARRAY", "%s of empty array", st->def->name), NULL;
        int64_t best = 0;
        if (!get_i64(in->items[0], "input element", &best, e)) return NULL;
        for (size_t i = 1; i < in->alen; i++) {
            int64_t v = 0;
            if (!get_i64(in->items[i], "input element", &v, e)) return NULL;
            if (k == OP_MIN ? v < best : v > best) best = v;
        }
        return jint(best);
    }
    case OP_RANGE: {
        int64_t start, end;
        if (!arg_i64(args, "start", &start, e)) return NULL;
        if (!arg_i64(args, "end", &end, e)) return NULL;
        JVal *out = jarr();
        if (start > end) return out;
        __int128 cnt = (__int128)end - (__int128)start + 1;
        for (__int128 i = 0; i < cnt; i++) jarr_push(out, jint((int64_t)((__int128)start + i)));
        return out;
    }
    case OP_CONCAT: {
        const char *a, *b;
        size_t an, bn;
        if (!arg_str(args, "a", &a, &an, e)) return NULL;
        if (!arg_str(args, "b", &b, &bn, e)) return NULL;
        SB sb;
        sb_init(&sb);
        sb_putn(&sb, a, an);
        sb_putn(&sb, b, bn);
        char *s = sb_finish(&sb);
        return jstrn(s, sb.len);
    }
    case OP_SPLIT: {
        const char *s, *sep;
        size_t n, sepn;
        if (!arg_str(args, "input", &s, &n, e)) return NULL;
        if (!arg_str(args, "sep", &sep, &sepn, e)) return NULL;
        JVal *out = jarr();
        split_string(s, n, sep, sepn, out);
        return out;
    }
    case OP_JOIN: {
        /* note: input is an array of strings */
        JVal *in;
        if (!arg_arr(args, "input", &in, e)) return NULL;
        const char *sep;
        size_t sepn;
        if (!arg_str(args, "sep", &sep, &sepn, e)) return NULL;
        SB sb;
        sb_init(&sb);
        for (size_t i = 0; i < in->alen; i++) {
            if (in->items[i]->t != J_STR)
                return err_set(e, "INVALID_ARG", "join input must be an array of strings"), NULL;
            if (i) sb_putn(&sb, sep, sepn);
            sb_putn(&sb, in->items[i]->s, in->items[i]->slen);
        }
        char *s = sb_finish(&sb);
        return jstrn(s, sb.len);
    }
    case OP_UPPERCASE:
    case OP_LOWERCASE:
    case OP_TRIM: {
        const char *s;
        size_t n;
        if (!arg_str(args, "input", &s, &n, e)) return NULL;
        if (k == OP_UPPERCASE) {
            char *buf = xmalloc(n + 1);
            for (size_t i = 0; i < n; i++) {
                char c = s[i];
                if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
                buf[i] = c;
            }
            buf[n] = '\0';
            return jstrn(buf, n);
        }
        if (k == OP_LOWERCASE) {
            char *buf = xmalloc(n + 1);
            for (size_t i = 0; i < n; i++) {
                char c = s[i];
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                buf[i] = c;
            }
            buf[n] = '\0';
            return jstrn(buf, n);
        }
        size_t a = 0, b = n;
        while (a < b && is_ascii_ws(s[a])) a++;
        while (b > a && is_ascii_ws(s[b - 1])) b--;
        return jstrn(s + a, b - a);
    }
    case OP_REPLACE: {
        const char *s, *search, *repl;
        size_t n, schn, rn;
        if (!arg_str(args, "input", &s, &n, e)) return NULL;
        if (!arg_str(args, "search", &search, &schn, e)) return NULL;
        if (!arg_str(args, "replace", &repl, &rn, e)) return NULL;
        if (schn == 0) return err_set(e, "INVALID_ARG", "replace search must not be empty"), NULL;
        SB sb;
        sb_init(&sb);
        size_t i = 0;
        while (i + schn <= n) {
            if (memcmp(s + i, search, schn) == 0) {
                sb_putn(&sb, repl, rn);
                i += schn;
            } else {
                sb_putc(&sb, s[i]);
                i++;
            }
        }
        if (i < n) sb_putn(&sb, s + i, n - i);
        char *p = sb_finish(&sb);
        return jstrn(p, sb.len);
    }
    case OP_LENGTH: {
        const char *s;
        size_t n;
        if (!arg_str(args, "input", &s, &n, e)) return NULL;
        int64_t cnt = 0;
        for (size_t i = 0; i < n; i++)
            if ((((unsigned char)s[i]) & 0xC0) != 0x80) cnt++;
        return jint(cnt);
    }
    case OP_MAP: {
        JVal *in;
        if (!arg_arr(args, "input", &in, e)) return NULL;
        JVal *trv = arg_any(args, "transform", e);
        if (!trv) return NULL;
        Transform tr;
        if (!parse_transform(trv, &tr, e)) return NULL;
        JVal *out = jarr();
        for (size_t i = 0; i < in->alen; i++) {
            JVal *r = NULL;
            if (!tr_apply(&tr, in->items[i], &r, e)) return NULL;
            jarr_push(out, r);
        }
        return out;
    }
    case OP_FILTER: {
        JVal *in;
        if (!arg_arr(args, "input", &in, e)) return NULL;
        JVal *pv = arg_any(args, "predicate", e);
        if (!pv) return NULL;
        Predicate pr;
        if (!parse_predicate(pv, &pr, e)) return NULL;
        JVal *out = jarr();
        for (size_t i = 0; i < in->alen; i++) {
            bool keep = false;
            if (!pred_apply(&pr, in->items[i], &keep, e)) return NULL;
            if (keep) jarr_push(out, in->items[i]);
        }
        return out;
    }
    case OP_REDUCE: {
        JVal *in;
        if (!arg_arr(args, "input", &in, e)) return NULL;
        const char *opn;
        size_t opnlen;
        if (!arg_str(args, "op", &opn, &opnlen, e)) return NULL;
        OpKind bin;
        if (strcmp(opn, "add") == 0) bin = OP_ADD;
        else if (strcmp(opn, "subtract") == 0) bin = OP_SUBTRACT;
        else if (strcmp(opn, "multiply") == 0) bin = OP_MULTIPLY;
        else return err_set(e, "INVALID_ARG", "unsupported reduce op '%s'", opn), NULL;
        int64_t acc;
        JVal *iv = jobj_get(args, "initial");
        if (iv) {
            if (!get_i64(iv, "initial", &acc, e)) return NULL;
        } else {
            acc = (bin == OP_MULTIPLY) ? 1 : 0;
        }
        for (size_t i = 0; i < in->alen; i++) {
            int64_t v = 0;
            if (!get_i64(in->items[i], "input element", &v, e)) return NULL;
            if (!apply_bin(bin, acc, v, &acc, e)) return NULL;
        }
        return jint(acc);
    }
    case OP_SORT: {
        JVal *in;
        if (!arg_arr(args, "input", &in, e)) return NULL;
        JVal *out = jarr();
        size_t n = in->alen;
        JVal **tmp = xmalloc((n ? n : 1) * sizeof(JVal *));
        for (size_t i = 0; i < n; i++) jarr_push(out, in->items[i]);
        msort(out->items, tmp, n);
        free(tmp);
        return out;
    }
    case OP_REVERSE: {
        JVal *in;
        if (!arg_arr(args, "input", &in, e)) return NULL;
        JVal *out = jarr();
        for (size_t i = in->alen; i > 0; i--) jarr_push(out, in->items[i - 1]);
        return out;
    }
    case OP_UNIQUE: {
        JVal *in;
        if (!arg_arr(args, "input", &in, e)) return NULL;
        JVal *out = jarr();
        for (size_t i = 0; i < in->alen; i++) {
            bool dup = false;
            for (size_t j = 0; j < out->alen; j++) {
                if (jeq(out->items[j], in->items[i])) { dup = true; break; }
            }
            if (!dup) jarr_push(out, in->items[i]);
        }
        return out;
    }
    case OP_SLICE: {
        JVal *in;
        if (!arg_arr(args, "input", &in, e)) return NULL;
        int64_t start;
        if (!arg_i64(args, "start", &start, e)) return NULL;
        JVal *ev = jobj_get(args, "end");
        bool has_end = ev != NULL;
        int64_t endv = 0;
        if (has_end && !get_i64(ev, "end", &endv, e)) return NULL;
        __int128 n = (__int128)in->alen;
        __int128 s = (__int128)start;
        __int128 en = has_end ? (__int128)endv : n;
        if (s < 0) s += n;
        if (en < 0) en += n;
        if (s < 0) s = 0;
        if (s > n) s = n;
        if (en < 0) en = 0;
        if (en > n) en = n;
        if (s > en) s = en;
        JVal *out = jarr();
        for (__int128 i = s; i < en; i++) jarr_push(out, in->items[(size_t)i]);
        return out;
    }
    case OP_FLATTEN: {
        JVal *in;
        if (!arg_arr(args, "input", &in, e)) return NULL;
        JVal *out = jarr();
        for (size_t i = 0; i < in->alen; i++) {
            JVal *el = in->items[i];
            if (el->t == J_ARR) {
                for (size_t j = 0; j < el->alen; j++) jarr_push(out, el->items[j]);
            } else {
                jarr_push(out, el);
            }
        }
        return out;
    }
    case OP_GET: {
        JVal *in;
        if (!arg_obj(args, "input", &in, e)) return NULL;
        const char *key;
        size_t kn;
        if (!arg_str(args, "key", &key, &kn, e)) return NULL;
        JVal *v = jobj_getn(in, key, kn);
        return v ? v : jnull();
    }
    case OP_SET: {
        JVal *in;
        if (!arg_obj(args, "input", &in, e)) return NULL;
        const char *key;
        size_t kn;
        if (!arg_str(args, "key", &key, &kn, e)) return NULL;
        JVal *val = arg_any(args, "value", e);
        if (!val) return NULL;
        JVal *out = jobj();
        for (size_t i = 0; i < in->olen; i++)
            jobj_put(out, in->keys[i], in->klens[i], in->vals[i]);
        jobj_put(out, key, kn, val);
        return out;
    }
    case OP_KEYS: {
        JVal *in;
        if (!arg_obj(args, "input", &in, e)) return NULL;
        size_t *idx = sorted_indices(in);
        JVal *out = jarr();
        for (size_t i = 0; i < in->olen; i++)
            jarr_push(out, jstrn(in->keys[idx[i]], in->klens[idx[i]]));
        free(idx);
        return out;
    }
    case OP_VALUES: {
        JVal *in;
        if (!arg_obj(args, "input", &in, e)) return NULL;
        size_t *idx = sorted_indices(in);
        JVal *out = jarr();
        for (size_t i = 0; i < in->olen; i++)
            jarr_push(out, in->vals[idx[i]]);
        free(idx);
        return out;
    }
    case OP_MERGE: {
        JVal *a, *b;
        if (!arg_obj(args, "a", &a, e)) return NULL;
        if (!arg_obj(args, "b", &b, e)) return NULL;
        JVal *out = jobj();
        for (size_t i = 0; i < a->olen; i++)
            jobj_put(out, a->keys[i], a->klens[i], a->vals[i]);
        for (size_t i = 0; i < b->olen; i++)
            jobj_put(out, b->keys[i], b->klens[i], b->vals[i]);
        return out;
    }
    case OP_PICK: {
        JVal *in;
        if (!arg_obj(args, "input", &in, e)) return NULL;
        JVal *keys;
        if (!arg_arr(args, "keys", &keys, e)) return NULL;
        JVal *tmp = jobj();
        for (size_t i = 0; i < keys->alen; i++) {
            if (keys->items[i]->t != J_STR)
                return err_set(e, "INVALID_ARG", "pick keys must be strings"), NULL;
            JVal *v = jobj_getn(in, keys->items[i]->s, keys->items[i]->slen);
            jobj_put(tmp, keys->items[i]->s, keys->items[i]->slen, v ? v : jnull());
        }
        size_t *idx = sorted_indices(tmp);
        JVal *out = jobj();
        for (size_t i = 0; i < tmp->olen; i++)
            jobj_put(out, tmp->keys[idx[i]], tmp->klens[idx[i]], tmp->vals[idx[i]]);
        free(idx);
        return out;
    }
    case OP_OMIT: {
        JVal *in;
        if (!arg_obj(args, "input", &in, e)) return NULL;
        JVal *keys;
        if (!arg_arr(args, "keys", &keys, e)) return NULL;
        for (size_t i = 0; i < keys->alen; i++)
            if (keys->items[i]->t != J_STR)
                return err_set(e, "INVALID_ARG", "omit keys must be strings"), NULL;
        JVal *out = jobj();
        for (size_t i = 0; i < in->olen; i++) {
            bool skip = false;
            for (size_t j = 0; j < keys->alen; j++) {
                if (keys->items[j]->slen == in->klens[i] &&
                    memcmp(keys->items[j]->s, in->keys[i], in->klens[i]) == 0) {
                    skip = true;
                    break;
                }
            }
            if (!skip) jobj_put(out, in->keys[i], in->klens[i], in->vals[i]);
        }
        return out;
    }
    case OP_CONSTANT: {
        JVal *v = arg_any(args, "value", e);
        return v;
    }
    case OP_EQUALS: {
        JVal *a = arg_any(args, "a", e);
        if (!a) return NULL;
        JVal *b = arg_any(args, "b", e);
        if (!b) return NULL;
        return jbool(jeq(a, b));
    }
    case OP_COMPARE: {
        JVal *a = arg_any(args, "a", e);
        if (!a) return NULL;
        JVal *b = arg_any(args, "b", e);
        if (!b) return NULL;
        const char *opn;
        size_t on;
        if (!arg_str(args, "op", &opn, &on, e)) return NULL;
        CmpKind c;
        if (!cmp_from_name(opn, &c) || c == CMP_EQ || c == CMP_NE)
            return err_set(e, "INVALID_ARG", "compare op must be gt|gte|lt|lte"), NULL;
        return jbool(cmp_eval(c, a, b));
    }
    case OP_AND: {
        JVal *a = arg_any(args, "a", e);
        if (!a) return NULL;
        JVal *b = arg_any(args, "b", e);
        if (!b) return NULL;
        return jbool(jtruthy(a) && jtruthy(b));
    }
    case OP_OR: {
        JVal *a = arg_any(args, "a", e);
        if (!a) return NULL;
        JVal *b = arg_any(args, "b", e);
        if (!b) return NULL;
        return jbool(jtruthy(a) || jtruthy(b));
    }
    case OP_NOT: {
        JVal *a = arg_any(args, "a", e);
        if (!a) return NULL;
        return jbool(!jtruthy(a));
    }
    case OP_TYPE: {
        JVal *v = arg_any(args, "value", e);
        if (!v) return NULL;
        return jstr(jtype_name(v));
    }
    case OP_FAIL_UNTIL: {
        int64_t failures;
        if (!arg_i64(args, "failures", &failures, e)) return NULL;
        if (failures < 0 || failures > 10)
            return err_set(e, "INVALID_ARG", "failures must be between 0 and 10"), NULL;
        JVal *val = arg_any(args, "value", e);
        if (!val) return NULL;
        if (attempt <= failures)
            return err_set(e, "TRANSIENT_FAILURE", "transient failure (attempt %d of %lld)",
                           attempt, (long long)failures), NULL;
        return val;
    }
    }
    return err_set(e, "INVALID_ARG", "unhandled op"), NULL;
}

/* ------------------------------------------------------------------ */
/* validation                                                          */
/* ------------------------------------------------------------------ */

static bool known_field(const JVal *o, size_t i, const char **fields, int n) {
    for (int j = 0; j < n; j++) {
        if (o->klens[i] == strlen(fields[j]) &&
            memcmp(o->keys[i], fields[j], o->klens[i]) == 0)
            return true;
    }
    return false;
}

static bool validate_cond(JVal *w, const char *stepid, WErr *e) {
    if (w->t == J_BOOL) return true;
    if (is_ref_obj(w)) return true;
    if (w->t != J_OBJ) return true; /* becomes INVALID_CONDITION at runtime */
    JVal *opv = jobj_get(w, "op");
    if (!opv || opv->t != J_STR)
        return vfail(e, "INVALID_WORKFLOW", stepid, "condition requires a string 'op'");
    const char *op = opv->s;
    CmpKind c;
    if (cmp_from_name(op, &c)) {
        const char *fields[] = {"op", "left", "right"};
        for (size_t i = 0; i < w->olen; i++)
            if (!known_field(w, i, fields, 3))
                return vfail(e, "INVALID_WORKFLOW", stepid,
                             "unknown condition field '%.*s'", (int)w->klens[i], w->keys[i]);
        if (!jobj_get(w, "left") || !jobj_get(w, "right"))
            return vfail(e, "INVALID_WORKFLOW", stepid, "condition requires 'left' and 'right'");
        return true;
    }
    if (strcmp(op, "all") == 0 || strcmp(op, "any") == 0) {
        const char *fields[] = {"op", "conditions"};
        for (size_t i = 0; i < w->olen; i++)
            if (!known_field(w, i, fields, 2))
                return vfail(e, "INVALID_WORKFLOW", stepid,
                             "unknown condition field '%.*s'", (int)w->klens[i], w->keys[i]);
        JVal *conds = jobj_get(w, "conditions");
        if (!conds || conds->t != J_ARR)
            return vfail(e, "INVALID_WORKFLOW", stepid, "'%s' requires a 'conditions' array", op);
        for (size_t i = 0; i < conds->alen; i++)
            if (!validate_cond(conds->items[i], stepid, e)) return false;
        return true;
    }
    if (strcmp(op, "not") == 0) {
        const char *fields[] = {"op", "condition"};
        for (size_t i = 0; i < w->olen; i++)
            if (!known_field(w, i, fields, 2))
                return vfail(e, "INVALID_WORKFLOW", stepid,
                             "unknown condition field '%.*s'", (int)w->klens[i], w->keys[i]);
        JVal *cnd = jobj_get(w, "condition");
        if (!cnd) return vfail(e, "INVALID_WORKFLOW", stepid, "'not' requires a 'condition'");
        return validate_cond(cnd, stepid, e);
    }
    return vfail(e, "INVALID_WORKFLOW", stepid, "unknown condition operator '%s'", op);
}

static bool validate_step(JVal *s, size_t index, WErr *e) {
    if (s->t != J_OBJ)
        return vfail(e, "INVALID_WORKFLOW", NULL, "step %zu is not an object", index);

    JVal *idv = jobj_get(s, "id");
    const char *stepid = (idv && idv->t == J_STR) ? idv->s : NULL;

    const char *fields[] = {"id", "op", "args", "depends_on", "when", "retry"};
    for (size_t i = 0; i < s->olen; i++) {
        if (!known_field(s, i, fields, 6))
            return vfail(e, "INVALID_WORKFLOW", stepid,
                         "unknown step field '%.*s'", (int)s->klens[i], s->keys[i]);
    }

    if (!idv)
        return vfail(e, "INVALID_WORKFLOW", stepid, "step %zu is missing 'id'", index);
    if (idv->t != J_STR)
        return vfail(e, "INVALID_WORKFLOW", NULL, "step %zu has a non-string 'id'", index);
    if (!valid_id(idv->s, idv->slen))
        return vfail(e, "INVALID_WORKFLOW", stepid, "invalid step id '%s'", idv->s);

    JVal *opv = jobj_get(s, "op");
    if (!opv || opv->t != J_STR)
        return vfail(e, "INVALID_WORKFLOW", stepid, "step '%s' requires a string 'op'", idv->s);
    const OpDef *def = find_op(opv->s);
    if (!def)
        return vfail(e, "INVALID_WORKFLOW", stepid, "unknown op '%s'", opv->s);

    JVal *argsv = jobj_get(s, "args");
    if (argsv && argsv->t != J_OBJ)
        return vfail(e, "INVALID_WORKFLOW", stepid, "'args' must be an object");

    JVal *depv = jobj_get(s, "depends_on");
    if (depv) {
        if (depv->t != J_ARR)
            return vfail(e, "INVALID_WORKFLOW", stepid, "'depends_on' must be an array");
        for (size_t i = 0; i < depv->alen; i++)
            if (depv->items[i]->t != J_STR)
                return vfail(e, "INVALID_WORKFLOW", stepid,
                             "'depends_on' entries must be strings");
    }

    JVal *wv = jobj_get(s, "when");
    if (wv && !validate_cond(wv, stepid, e)) return false;

    JVal *rv = jobj_get(s, "retry");
    if (rv) {
        if (rv->t != J_OBJ)
            return vfail(e, "INVALID_WORKFLOW", stepid, "'retry' must be an object");
        for (size_t i = 0; i < rv->olen; i++) {
            if (!(rv->klens[i] == 12 && memcmp(rv->keys[i], "max_attempts", 12) == 0))
                return vfail(e, "INVALID_WORKFLOW", stepid,
                             "unknown retry field '%.*s'", (int)rv->klens[i], rv->keys[i]);
        }
        JVal *mav = jobj_get(rv, "max_attempts");
        if (mav) {
            if (mav->t != J_NUM || !mav->is_int)
                return vfail(e, "INVALID_WORKFLOW", stepid,
                             "'max_attempts' must be an integer literal");
            if (mav->i < 1 || mav->i > 5)
                return vfail(e, "INVALID_WORKFLOW", stepid,
                             "'max_attempts' must be between 1 and 5");
        }
    }

    return true;
}

static void compute_reach(Engine *en, size_t i, bool *computed) {
    if (computed[i]) return;
    computed[i] = true;
    Step *st = &en->steps[i];
    for (size_t d = 0; d < st->ndeps; d++) {
        size_t j = st->deps[d];
        en->reach[i * en->n + j] = true;
        compute_reach(en, j, computed);
        for (size_t k = 0; k < en->n; k++)
            if (en->reach[j * en->n + k]) en->reach[i * en->n + k] = true;
    }
}

static bool cycle_dfs(Engine *en, size_t i, int *state) {
    state[i] = 1;
    for (size_t d = 0; d < en->steps[i].ndeps; d++) {
        size_t j = en->steps[i].deps[d];
        if (state[j] == 1) return true;
        if (state[j] == 0 && cycle_dfs(en, j, state)) return true;
    }
    state[i] = 2;
    return false;
}

static bool build_engine(JVal *doc, Engine *en, WErr *e) {
    if (doc->t != J_OBJ)
        return vfail(e, "INVALID_WORKFLOW", NULL, "workflow document must be an object");

    const char *topfields[] = {"version", "steps"};
    for (size_t i = 0; i < doc->olen; i++) {
        if (!known_field(doc, i, topfields, 2))
            return vfail(e, "INVALID_WORKFLOW", NULL,
                         "unknown top-level field '%.*s'", (int)doc->klens[i], doc->keys[i]);
    }

    JVal *ver = jobj_get(doc, "version");
    if (!ver)
        return vfail(e, "INVALID_WORKFLOW", NULL, "missing 'version'");
    if (ver->t != J_NUM || !ver->is_int || ver->i != 1)
        return vfail(e, "INVALID_WORKFLOW", NULL, "'version' must be the integer literal 1");

    JVal *steps = jobj_get(doc, "steps");
    if (!steps)
        return vfail(e, "INVALID_WORKFLOW", NULL, "missing 'steps'");
    if (steps->t != J_ARR)
        return vfail(e, "INVALID_WORKFLOW", NULL, "'steps' must be an array");

    size_t n = steps->alen;
    en->n = n;
    en->steps = xmalloc((n ? n : 1) * sizeof(Step));
    memset(en->steps, 0, (n ? n : 1) * sizeof(Step));

    for (size_t i = 0; i < n; i++) {
        JVal *s = steps->items[i];
        if (!validate_step(s, i, e)) return false;
        Step *st = &en->steps[i];
        JVal *idv = jobj_get(s, "id");
        st->id = xstrndup(idv->s, idv->slen);
        /* duplicate id check */
        for (size_t j = 0; j < i; j++) {
            if (strcmp(en->steps[j].id, st->id) == 0)
                return vfail(e, "INVALID_WORKFLOW", st->id, "duplicate step id '%s'", st->id);
        }
        JVal *opv = jobj_get(s, "op");
        st->def = find_op(opv->s);
        st->kind = st->def->kind;
        JVal *argsv = jobj_get(s, "args");
        st->args = argsv ? argsv : jobj();
        JVal *depv = jobj_get(s, "depends_on");
        if (depv && depv->alen > 0) {
            st->ndeps = depv->alen;
            st->depnames = xmalloc(depv->alen * sizeof(char *));
            st->deplens = xmalloc(depv->alen * sizeof(size_t));
            for (size_t j = 0; j < depv->alen; j++) {
                st->depnames[j] = depv->items[j]->s;
                st->deplens[j] = depv->items[j]->slen;
            }
        }
        st->when = jobj_get(s, "when");
        int ma = 1;
        JVal *rv = jobj_get(s, "retry");
        if (rv) {
            JVal *mav = jobj_get(rv, "max_attempts");
            if (mav) ma = (int)mav->i;
        }
        st->max_attempts = ma;
    }

    /* resolve dependency names to indices */
    for (size_t i = 0; i < n; i++) {
        Step *st = &en->steps[i];
        st->deps = xmalloc((st->ndeps ? st->ndeps : 1) * sizeof(size_t));
        for (size_t d = 0; d < st->ndeps; d++) {
            size_t idx = SIZE_MAX;
            for (size_t j = 0; j < n; j++) {
                if (strlen(en->steps[j].id) == st->deplens[d] &&
                    memcmp(en->steps[j].id, st->depnames[d], st->deplens[d]) == 0) {
                    idx = j;
                    break;
                }
            }
            if (idx == SIZE_MAX)
                return vfail(e, "UNKNOWN_DEPENDENCY", st->id,
                             "step '%s' depends on unknown step '%.*s'",
                             st->id, (int)st->deplens[d], st->depnames[d]);
            st->deps[d] = idx;
        }
    }

    /* cycle detection */
    int *state = xmalloc((n ? n : 1) * sizeof(int));
    memset(state, 0, (n ? n : 1) * sizeof(int));
    for (size_t i = 0; i < n; i++) {
        if (state[i] == 0 && cycle_dfs(en, i, state)) {
            free(state);
            return vfail(e, "CYCLIC_DEPENDENCY", NULL, "dependency cycle detected");
        }
    }
    free(state);

    /* transitive closure */
    en->reach = xmalloc((n ? n * n : 1) * sizeof(bool));
    memset(en->reach, 0, (n ? n * n : 1) * sizeof(bool));
    bool *computed = xmalloc((n ? n : 1) * sizeof(bool));
    memset(computed, 0, (n ? n : 1) * sizeof(bool));
    for (size_t i = 0; i < n; i++) compute_reach(en, i, computed);
    free(computed);

    /* runtime state */
    en->done = xmalloc((n ? n : 1) * sizeof(bool));
    en->skipped = xmalloc((n ? n : 1) * sizeof(bool));
    en->values = xmalloc((n ? n : 1) * sizeof(JVal *));
    en->attempts = xmalloc((n ? n : 1) * sizeof(int));
    memset(en->done, 0, (n ? n : 1) * sizeof(bool));
    memset(en->skipped, 0, (n ? n : 1) * sizeof(bool));
    memset(en->values, 0, (n ? n : 1) * sizeof(JVal *));
    memset(en->attempts, 0, (n ? n : 1) * sizeof(int));
    en->results_obj = jobj();
    en->order = jarr();
    en->skipped_arr = jarr();

    return true;
}

/* ------------------------------------------------------------------ */
/* response output                                                     */
/* ------------------------------------------------------------------ */

static void emit(const char *status, JVal *results, JVal *order, JVal *skipped,
                 const char *code, const char *step, const char *msg) {
    JVal *o = jobj();
    jobj_put(o, "status", 6, jstr(status));
    jobj_put(o, "results", 7, results ? results : jobj());
    jobj_put(o, "execution_order", 15, order ? order : jarr());
    jobj_put(o, "skipped", 7, skipped ? skipped : jarr());
    if (code) {
        JVal *er = jobj();
        jobj_put(er, "code", 4, jstr(code));
        jobj_put(er, "step", 4, step ? jstr(step) : jnull());
        jobj_put(er, "message", 7, jstr(msg ? msg : ""));
        jobj_put(o, "error", 5, er);
    } else {
        jobj_put(o, "error", 5, jnull());
    }
    jwrite(stdout, o);
    fputc('\n', stdout);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* execution                                                           */
/* ------------------------------------------------------------------ */

static int execute(Engine *en) {
    size_t n = en->n;
    for (;;) {
        size_t pick = SIZE_MAX;
        for (size_t i = 0; i < n; i++) {
            if (en->done[i]) continue;
            bool ready = true;
            for (size_t d = 0; d < en->steps[i].ndeps; d++) {
                if (!en->done[en->steps[i].deps[d]]) { ready = false; break; }
            }
            if (ready) { pick = i; break; }
        }
        if (pick == SIZE_MAX) break;

        Step *st = &en->steps[pick];
        WErr e;
        e.code = NULL;
        e.step = st->id;
        e.msg[0] = '\0';

        if (st->when) {
            bool run = false;
            if (!eval_cond(en, pick, st->when, &run, &e)) {
                emit("failed", en->results_obj, en->order, en->skipped_arr,
                     e.code ? e.code : "INVALID_CONDITION", st->id, e.msg);
                return 1;
            }
            if (!run) {
                en->done[pick] = true;
                en->skipped[pick] = true;
                en->values[pick] = jnull();
                jobj_put(en->results_obj, st->id, strlen(st->id), jnull());
                jarr_push(en->order, jstr(st->id));
                jarr_push(en->skipped_arr, jstr(st->id));
                continue;
            }
        }

        JVal *rargs = resolve_value(en, pick, st->args, &e);
        if (!rargs) {
            emit("failed", en->results_obj, en->order, en->skipped_arr,
                 e.code ? e.code : "INVALID_REFERENCE", st->id, e.msg);
            return 1;
        }

        JVal *result = NULL;
        bool failed = false;
        for (int attempt = 1; attempt <= st->max_attempts; attempt++) {
            en->attempts[pick]++;
            e.code = NULL;
            e.msg[0] = '\0';
            result = exec_op(en, pick, rargs, en->attempts[pick], &e);
            if (result) { failed = false; break; }
            failed = true;
            if (e.code && strcmp(e.code, "TRANSIENT_FAILURE") == 0 &&
                attempt < st->max_attempts) {
                continue;
            }
            break;
        }
        if (failed) {
            const char *code = e.code ? e.code : "STEP_FAILED";
            if (strcmp(code, "TRANSIENT_FAILURE") == 0) code = "STEP_FAILED";
            emit("failed", en->results_obj, en->order, en->skipped_arr,
                 code, st->id, e.msg);
            return 1;
        }

        en->done[pick] = true;
        en->values[pick] = result;
        jobj_put(en->results_obj, st->id, strlen(st->id), result);
        jarr_push(en->order, jstr(st->id));
    }

    emit("completed", en->results_obj, en->order, en->skipped_arr, NULL, NULL, NULL);
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static char *read_all(FILE *f, size_t *outlen) {
    size_t cap = 65536, len = 0;
    char *buf = xmalloc(cap);
    for (;;) {
        if (len == cap) {
            cap *= 2;
            buf = xrealloc(buf, cap);
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
    *outlen = len;
    return buf;
}

int main(int argc, char **argv) {
    if (argc != 3 || strcmp(argv[1], "run") != 0) {
        emit("failed", jobj(), jarr(), jarr(), "INVALID_WORKFLOW", NULL,
             "usage: workflow run <workflow.json>");
        return 2;
    }

    FILE *f = NULL;
    bool is_stdin = strcmp(argv[2], "-") == 0;
    if (is_stdin) {
        f = stdin;
    } else {
        f = fopen(argv[2], "rb");
        if (!f) {
            emit("failed", jobj(), jarr(), jarr(), "INVALID_WORKFLOW", NULL,
                 "cannot open workflow file");
            return 2;
        }
    }

    size_t len = 0;
    char *buf = read_all(f, &len);
    if (!is_stdin) fclose(f);
    if (!buf) {
        emit("failed", jobj(), jarr(), jarr(), "INVALID_WORKFLOW", NULL,
             "cannot read workflow input");
        return 2;
    }

    char perr[256];
    JVal *doc = jparse(buf, len, perr, sizeof(perr));
    if (!doc) {
        emit("failed", jobj(), jarr(), jarr(), "INVALID_WORKFLOW", NULL,
             perr[0] ? perr : "invalid JSON");
        return 2;
    }

    Engine en;
    memset(&en, 0, sizeof(en));
    WErr ve;
    ve.code = NULL;
    ve.step = NULL;
    ve.msg[0] = '\0';
    if (!build_engine(doc, &en, &ve)) {
        emit("failed", jobj(), jarr(), jarr(), ve.code, ve.step, ve.msg);
        return 2;
    }

    return execute(&en);
}
