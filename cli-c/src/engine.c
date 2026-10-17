#include "json.h"

#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* error codes                                                         */
/* ------------------------------------------------------------------ */

#define E_INVALID_WORKFLOW "INVALID_WORKFLOW"
#define E_UNKNOWN_DEPENDENCY "UNKNOWN_DEPENDENCY"
#define E_CYCLIC_DEPENDENCY "CYCLIC_DEPENDENCY"
#define E_INVALID_REFERENCE "INVALID_REFERENCE"
#define E_SKIPPED_REFERENCE "SKIPPED_REFERENCE"
#define E_INVALID_ARG "INVALID_ARG"
#define E_INVALID_CONDITION "INVALID_CONDITION"
#define E_ARITHMETIC_OVERFLOW "ARITHMETIC_OVERFLOW"
#define E_DIVISION_BY_ZERO "DIVISION_BY_ZERO"
#define E_EMPTY_ARRAY "EMPTY_ARRAY"
#define E_TRANSIENT_FAILURE "TRANSIENT_FAILURE"
#define E_STEP_FAILED "STEP_FAILED"
#define E_INTERNAL_ERROR "INTERNAL_ERROR"

typedef struct {
    const char *code;
    const char *step;
    char msg[256];
} Err;

static void err_set(Err *e, const char *code, const char *step, const char *fmt, ...)
{
    e->code = code;
    e->step = step;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->msg, sizeof(e->msg), fmt, ap);
    va_end(ap);
}

static int exit_for_code(const char *code)
{
    if (!code)
        return 0;
    if (strcmp(code, E_INVALID_WORKFLOW) == 0 ||
        strcmp(code, E_UNKNOWN_DEPENDENCY) == 0 ||
        strcmp(code, E_CYCLIC_DEPENDENCY) == 0)
        return 2;
    if (strcmp(code, E_INTERNAL_ERROR) == 0)
        return 3;
    return 1;
}

/* ------------------------------------------------------------------ */
/* workflow model                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *id; /* NUL-terminated; valid ASCII identifier */
    const char *op;
    JVal *args;
    JVal *when;
    JVal *dep_arr; /* original depends_on array or NULL */
    int *deps;
    int ndeps;
    int max_attempts;
    int fail_attempts;
} Step;

typedef struct {
    JVal *doc;
    Step *steps;
    int n;
    JVal **results;
    unsigned char *done;
    unsigned char *skipped;
    JVal *results_obj;
    JVal *exec_order;
    JVal *skipped_list;
} Ctx;

static int find_step_len(const Ctx *ctx, const char *id, size_t len)
{
    for (int i = 0; i < ctx->n; i++) {
        size_t l = strlen(ctx->steps[i].id);
        if (l == len && (len == 0 || memcmp(ctx->steps[i].id, id, len) == 0))
            return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* small string buffer                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    char *s;
    size_t n, cap;
} SB;

static void sb_put(SB *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        while (b->n + n + 1 > b->cap)
            b->cap = b->cap ? b->cap * 2 : 32;
        char *p = realloc(b->s, b->cap);
        if (!p)
            jv_oom();
        b->s = p;
    }
    if (n)
        memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = '\0';
}

static char *sb_take(SB *b, size_t *len)
{
    if (!b->s) {
        b->s = malloc(1);
        if (!b->s)
            jv_oom();
        b->s[0] = '\0';
        b->cap = 1;
    }
    if (len)
        *len = b->n;
    return b->s;
}

/* ------------------------------------------------------------------ */
/* UTF-8 helpers                                                       */
/* ------------------------------------------------------------------ */

static size_t cp_len(const unsigned char *p, size_t remaining)
{
    unsigned char c = p[0];
    size_t n;
    if (c < 0x80)
        return 1;
    if ((c & 0xE0) == 0xC0)
        n = 2;
    else if ((c & 0xF0) == 0xE0)
        n = 3;
    else if ((c & 0xF8) == 0xF0)
        n = 4;
    else
        n = 1;
    if (n > remaining)
        n = 1;
    return n;
}

static int jstr_eq_c(const JStr *s, const char *c)
{
    size_t n = strlen(c);
    return s->len == n && (n == 0 || memcmp(s->s, c, n) == 0);
}

/* ------------------------------------------------------------------ */
/* value helpers                                                       */
/* ------------------------------------------------------------------ */

static int is_self(const JVal *v)
{
    return v && v->type == J_OBJ && v->u.o.n == 1 &&
           jstr_eq_c(&v->u.o.k[0], "$self") && v->u.o.v[0]->type == J_BOOL &&
           v->u.o.v[0]->u.b;
}

static int get_i64(const JVal *v, int64_t *out)
{
    if (!v || v->type != J_NUM || !v->u.n.is_int)
        return -1;
    *out = v->u.n.i;
    return 0;
}

static JVal *mkcstr(const char *s) { return jv_cstr(s); }

/* ------------------------------------------------------------------ */
/* structural validation helpers                                       */
/* ------------------------------------------------------------------ */

static const char *const TOP_KEYS[] = { "version", "steps" };
static const char *const STEP_KEYS[] = {
    "id", "op", "args", "depends_on", "when", "retry"
};
static const char *const RETRY_KEYS[] = { "max_attempts" };
static const char *const COND_COMPARE_KEYS[] = { "op", "left", "right" };
static const char *const COND_ALLANY_KEYS[] = { "op", "conditions" };
static const char *const COND_NOT_KEYS[] = { "op", "condition" };

static const char *const OPS[] = {
    "add", "subtract", "multiply", "divide", "mod", "sum", "min", "max", "range",
    "concat", "split", "join", "uppercase", "lowercase", "trim", "replace",
    "length", "map", "filter", "reduce", "sort", "reverse", "unique", "slice",
    "flatten", "get", "set", "keys", "values", "merge", "pick", "omit",
    "constant", "equals", "compare", "and", "or", "not", "type", "fail_until"
};
#define NOPS ((int)(sizeof(OPS) / sizeof(OPS[0])))

static int op_known(const char *op)
{
    for (int i = 0; i < NOPS; i++)
        if (strcmp(OPS[i], op) == 0)
            return 1;
    return 0;
}

static int key_in(const JStr *k, const char *const *allowed, int n)
{
    for (int i = 0; i < n; i++) {
        size_t l = strlen(allowed[i]);
        if (k->len == l && (l == 0 || memcmp(k->s, allowed[i], l) == 0))
            return 1;
    }
    return 0;
}

static int check_keys(JVal *obj, const char *const *allowed, int n,
                      const char *code, const char *step, Err *e)
{
    if (obj->type != J_OBJ) {
        err_set(e, code, step, "expected object");
        return -1;
    }
    for (size_t i = 0; i < obj->u.o.n; i++) {
        if (!key_in(&obj->u.o.k[i], allowed, n)) {
            err_set(e, code, step, "unknown field '%s'", obj->u.o.k[i].s);
            return -1;
        }
    }
    return 0;
}

static int valid_id(const char *s)
{
    unsigned char c = (unsigned char)*s;
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'))
        return 0;
    for (const char *p = s + 1; *p; p++) {
        c = (unsigned char)*p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_'))
            return 0;
    }
    return 1;
}

static int validate_condition(JVal *c, const char *sid, Err *e)
{
    if (!c || c->type != J_OBJ) {
        err_set(e, E_INVALID_WORKFLOW, sid, "'when' must be an object");
        return -1;
    }
    JVal *opv = jv_obj_get(c, "op");
    if (!opv || opv->type != J_STR) {
        err_set(e, E_INVALID_WORKFLOW, sid, "condition requires string 'op'");
        return -1;
    }
    const char *op = opv->u.s.s;
    if (strcmp(op, "eq") == 0 || strcmp(op, "ne") == 0 ||
        strcmp(op, "gt") == 0 || strcmp(op, "gte") == 0 ||
        strcmp(op, "lt") == 0 || strcmp(op, "lte") == 0) {
        if (check_keys(c, COND_COMPARE_KEYS, 3, E_INVALID_WORKFLOW, sid, e) < 0)
            return -1;
        if (!jv_obj_has(c, "left") || !jv_obj_has(c, "right")) {
            err_set(e, E_INVALID_WORKFLOW, sid,
                    "condition '%s' requires 'left' and 'right'", op);
            return -1;
        }
        return 0;
    }
    if (strcmp(op, "all") == 0 || strcmp(op, "any") == 0) {
        if (check_keys(c, COND_ALLANY_KEYS, 2, E_INVALID_WORKFLOW, sid, e) < 0)
            return -1;
        JVal *cons = jv_obj_get(c, "conditions");
        if (!cons || cons->type != J_ARR) {
            err_set(e, E_INVALID_WORKFLOW, sid,
                    "condition '%s' requires array 'conditions'", op);
            return -1;
        }
        for (size_t i = 0; i < cons->u.a.n; i++) {
            if (validate_condition(cons->u.a.v[i], sid, e) < 0)
                return -1;
        }
        return 0;
    }
    if (strcmp(op, "not") == 0) {
        if (check_keys(c, COND_NOT_KEYS, 2, E_INVALID_WORKFLOW, sid, e) < 0)
            return -1;
        JVal *sub = jv_obj_get(c, "condition");
        if (!sub) {
            err_set(e, E_INVALID_WORKFLOW, sid,
                    "condition 'not' requires 'condition'");
            return -1;
        }
        return validate_condition(sub, sid, e);
    }
    err_set(e, E_INVALID_WORKFLOW, sid, "unknown condition operator '%s'", op);
    return -1;
}

static int has_dep(const Step *s, int target)
{
    for (int i = 0; i < s->ndeps; i++)
        if (s->deps[i] == target)
            return 1;
    return 0;
}

static int validate_workflow(Ctx *ctx, Err *e)
{
    JVal *doc = ctx->doc;
    if (doc->type != J_OBJ) {
        err_set(e, E_INVALID_WORKFLOW, NULL, "workflow must be a JSON object");
        return -1;
    }
    if (check_keys(doc, TOP_KEYS, 2, E_INVALID_WORKFLOW, NULL, e) < 0)
        return -1;

    JVal *ver = jv_obj_get(doc, "version");
    if (!ver || ver->type != J_NUM || !ver->u.n.is_int || ver->u.n.i != 1) {
        err_set(e, E_INVALID_WORKFLOW, NULL, "workflow version must be integer 1");
        return -1;
    }
    JVal *steps = jv_obj_get(doc, "steps");
    if (!steps || steps->type != J_ARR) {
        err_set(e, E_INVALID_WORKFLOW, NULL, "workflow requires a 'steps' array");
        return -1;
    }

    size_t n = steps->u.a.n;
    ctx->steps = calloc(n ? n : 1, sizeof(Step));
    ctx->results = calloc(n ? n : 1, sizeof(JVal *));
    ctx->done = calloc(n ? n : 1, 1);
    ctx->skipped = calloc(n ? n : 1, 1);
    if (!ctx->steps || !ctx->results || !ctx->done || !ctx->skipped)
        jv_oom();
    ctx->n = (int)n;

    for (size_t i = 0; i < n; i++) {
        JVal *sv = steps->u.a.v[i];
        Step *st = &ctx->steps[i];
        if (!sv || sv->type != J_OBJ) {
            err_set(e, E_INVALID_WORKFLOW, NULL, "step %zu is not an object", i);
            return -1;
        }
        JVal *idv = jv_obj_get(sv, "id");
        if (!idv || idv->type != J_STR) {
            err_set(e, E_INVALID_WORKFLOW, NULL,
                    "step %zu requires a string 'id'", i);
            return -1;
        }
        const char *sid = idv->u.s.s;
        if (!valid_id(sid)) {
            err_set(e, E_INVALID_WORKFLOW, sid, "invalid step id '%s'", sid);
            return -1;
        }
        st->id = sid;

        if (check_keys(sv, STEP_KEYS, 6, E_INVALID_WORKFLOW, sid, e) < 0)
            return -1;

        JVal *opv = jv_obj_get(sv, "op");
        if (!opv || opv->type != J_STR || !op_known(opv->u.s.s)) {
            err_set(e, E_INVALID_WORKFLOW, sid,
                    "step '%s' has a missing or unknown 'op'", sid);
            return -1;
        }
        st->op = opv->u.s.s;

        JVal *argsv = jv_obj_get(sv, "args");
        if (argsv) {
            if (argsv->type != J_OBJ) {
                err_set(e, E_INVALID_WORKFLOW, sid,
                        "step '%s' 'args' must be an object", sid);
                return -1;
            }
            st->args = argsv;
        } else {
            st->args = jv_obj();
        }

        JVal *depsv = jv_obj_get(sv, "depends_on");
        if (depsv) {
            if (depsv->type != J_ARR) {
                err_set(e, E_INVALID_WORKFLOW, sid,
                        "step '%s' 'depends_on' must be an array", sid);
                return -1;
            }
            for (size_t k = 0; k < depsv->u.a.n; k++) {
                if (depsv->u.a.v[k]->type != J_STR) {
                    err_set(e, E_INVALID_WORKFLOW, sid,
                            "step '%s' 'depends_on' entries must be strings", sid);
                    return -1;
                }
            }
            st->dep_arr = depsv;
        }

        JVal *whenv = jv_obj_get(sv, "when");
        if (whenv) {
            if (validate_condition(whenv, sid, e) < 0)
                return -1;
            st->when = whenv;
        }

        st->max_attempts = 1;
        JVal *retryv = jv_obj_get(sv, "retry");
        if (retryv) {
            if (retryv->type != J_OBJ) {
                err_set(e, E_INVALID_WORKFLOW, sid,
                        "step '%s' 'retry' must be an object", sid);
                return -1;
            }
            if (check_keys(retryv, RETRY_KEYS, 1, E_INVALID_WORKFLOW, sid, e) < 0)
                return -1;
            JVal *mav = jv_obj_get(retryv, "max_attempts");
            if (mav) {
                if (mav->type != J_NUM || !mav->u.n.is_int ||
                    mav->u.n.i < 1 || mav->u.n.i > 5) {
                    err_set(e, E_INVALID_WORKFLOW, sid,
                            "step '%s' retry.max_attempts must be 1..5", sid);
                    return -1;
                }
                st->max_attempts = (int)mav->u.n.i;
            }
        }
    }

    /* unique ids */
    for (int i = 0; i < ctx->n; i++) {
        for (int j = 0; j < i; j++) {
            if (strcmp(ctx->steps[i].id, ctx->steps[j].id) == 0) {
                err_set(e, E_INVALID_WORKFLOW, ctx->steps[i].id,
                        "duplicate step id '%s'", ctx->steps[i].id);
                return -1;
            }
        }
    }

    /* dependencies exist */
    for (int i = 0; i < ctx->n; i++) {
        Step *st = &ctx->steps[i];
        int nd = st->dep_arr ? (int)st->dep_arr->u.a.n : 0;
        st->deps = calloc(nd ? (size_t)nd : 1, sizeof(int));
        if (!st->deps)
            jv_oom();
        st->ndeps = nd;
        int write = 0;
        for (int k = 0; k < nd; k++) {
            JStr *id = &st->dep_arr->u.a.v[k]->u.s;
            int idx = find_step_len(ctx, id->s, id->len);
            if (idx < 0) {
                err_set(e, E_UNKNOWN_DEPENDENCY, st->id,
                        "step '%s' depends on unknown step '%s'", st->id, id->s);
                return -1;
            }
            int dup = 0;
            for (int q = 0; q < write; q++) {
                if (st->deps[q] == idx) {
                    dup = 1;
                    break;
                }
            }
            if (!dup)
                st->deps[write++] = idx;
        }
        st->ndeps = write;
    }

    /* cycle detection (Kahn) */
    {
        int *indeg = calloc(ctx->n ? (size_t)ctx->n : 1, sizeof(int));
        unsigned char *removed = calloc(ctx->n ? (size_t)ctx->n : 1, 1);
        if (!indeg || !removed)
            jv_oom();
        for (int i = 0; i < ctx->n; i++)
            indeg[i] = ctx->steps[i].ndeps;
        int processed = 0;
        for (int iter = 0; iter < ctx->n; iter++) {
            int found = -1;
            for (int i = 0; i < ctx->n; i++) {
                if (!removed[i] && indeg[i] == 0) {
                    found = i;
                    break;
                }
            }
            if (found < 0)
                break;
            removed[found] = 1;
            processed++;
            for (int j = 0; j < ctx->n; j++) {
                if (!removed[j] && has_dep(&ctx->steps[j], found))
                    indeg[j]--;
            }
        }
        free(indeg);
        free(removed);
        if (processed != ctx->n) {
            err_set(e, E_CYCLIC_DEPENDENCY, NULL,
                    "workflow contains a dependency cycle");
            return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* dependency / reference resolution                                   */
/* ------------------------------------------------------------------ */

static int is_transitive_dep(const Ctx *ctx, int from, int target)
{
    if (ctx->n == 0)
        return 0;
    unsigned char *seen = calloc((size_t)ctx->n, 1);
    int *stack = calloc((size_t)ctx->n, sizeof(int));
    if (!seen || !stack)
        jv_oom();
    int sp = 0, found = 0;
    seen[from] = 1;
    for (int i = 0; i < ctx->steps[from].ndeps; i++) {
        int d = ctx->steps[from].deps[i];
        if (d == target) {
            found = 1;
            break;
        }
        if (!seen[d]) {
            seen[d] = 1;
            stack[sp++] = d;
        }
    }
    while (!found && sp > 0) {
        int cur = stack[--sp];
        for (int i = 0; i < ctx->steps[cur].ndeps; i++) {
            int d = ctx->steps[cur].deps[i];
            if (d == target) {
                found = 1;
                break;
            }
            if (!seen[d]) {
                seen[d] = 1;
                stack[sp++] = d;
            }
        }
    }
    free(seen);
    free(stack);
    return found;
}

static int resolve_ref(Ctx *ctx, const char *ref, int cur, JVal **out, Err *e)
{
    const char *dot = strchr(ref, '.');
    size_t first_len = dot ? (size_t)(dot - ref) : strlen(ref);
    if (first_len == 0) {
        err_set(e, E_INVALID_REFERENCE, ctx->steps[cur].id,
                "empty reference target");
        return -1;
    }
    int idx = find_step_len(ctx, ref, first_len);
    if (idx < 0) {
        err_set(e, E_INVALID_REFERENCE, ctx->steps[cur].id,
                "reference '%s' targets an unknown step", ref);
        return -1;
    }
    if (idx == cur) {
        err_set(e, E_INVALID_REFERENCE, ctx->steps[cur].id,
                "self reference '%s'", ref);
        return -1;
    }
    if (!is_transitive_dep(ctx, cur, idx)) {
        err_set(e, E_INVALID_REFERENCE, ctx->steps[cur].id,
                "reference '%s' is not a dependency", ref);
        return -1;
    }
    if (ctx->skipped[idx]) {
        err_set(e, E_SKIPPED_REFERENCE, ctx->steps[cur].id,
                "reference '%s' targets skipped step '%s'", ref,
                ctx->steps[idx].id);
        return -1;
    }
    if (!ctx->done[idx] || !ctx->results[idx]) {
        err_set(e, E_INVALID_REFERENCE, ctx->steps[cur].id,
                "reference '%s' targets an unfinished step", ref);
        return -1;
    }
    JVal *v = ctx->results[idx];
    const char *p = ref + first_len;
    while (*p == '.') {
        p++;
        const char *next = strchr(p, '.');
        size_t seglen = next ? (size_t)(next - p) : strlen(p);
        if (seglen == 0) {
            err_set(e, E_INVALID_REFERENCE, ctx->steps[cur].id,
                    "empty path segment in '%s'", ref);
            return -1;
        }
        if (v->type == J_OBJ) {
            JVal *nv = jv_obj_get_len(v, p, seglen);
            if (!nv) {
                err_set(e, E_INVALID_REFERENCE, ctx->steps[cur].id,
                        "reference '%s' has a missing key", ref);
                return -1;
            }
            v = nv;
        } else if (v->type == J_ARR) {
            size_t ix = 0;
            int ok = seglen > 0;
            for (size_t k = 0; k < seglen; k++) {
                if (p[k] < '0' || p[k] > '9') {
                    ok = 0;
                    break;
                }
                if (ix > (SIZE_MAX - (size_t)(p[k] - '0')) / 10) {
                    ok = 0;
                    break;
                }
                ix = ix * 10 + (size_t)(p[k] - '0');
            }
            if (!ok || ix >= v->u.a.n) {
                err_set(e, E_INVALID_REFERENCE, ctx->steps[cur].id,
                        "reference '%s' has a bad array index", ref);
                return -1;
            }
            v = v->u.a.v[ix];
        } else {
            err_set(e, E_INVALID_REFERENCE, ctx->steps[cur].id,
                    "reference '%s' traverses a non-container", ref);
            return -1;
        }
        p += seglen;
    }
    *out = v;
    return 0;
}

/* Returns a new value with references resolved (containers cloned). */
static JVal *resolve_value(Ctx *ctx, JVal *v, int cur, Err *e, int *ok)
{
    if (!v)
        return jv_null();
    if (jv_is_ref(v)) {
        JVal *out = NULL;
        if (resolve_ref(ctx, v->u.o.v[0]->u.s.s, cur, &out, e) < 0) {
            *ok = 0;
            return NULL;
        }
        return out;
    }
    if (v->type == J_ARR) {
        JVal *arr = jv_arr();
        for (size_t i = 0; i < v->u.a.n; i++) {
            JVal *r = resolve_value(ctx, v->u.a.v[i], cur, e, ok);
            if (!*ok)
                return NULL;
            jv_arr_push(arr, r);
        }
        return arr;
    }
    if (v->type == J_OBJ) {
        JVal *obj = jv_obj();
        for (size_t i = 0; i < v->u.o.n; i++) {
            JVal *r = resolve_value(ctx, v->u.o.v[i], cur, e, ok);
            if (!*ok)
                return NULL;
            jv_obj_append(obj, v->u.o.k[i].s, v->u.o.k[i].len, r);
        }
        return obj;
    }
    return v;
}

/* ------------------------------------------------------------------ */
/* conditions                                                          */
/* ------------------------------------------------------------------ */

static JVal *eval_condition(Ctx *ctx, JVal *c, int cur, Err *e, int *ok);

static JVal *eval_comparison(Ctx *ctx, JVal *c, int cur, Err *e, int *ok)
{
    const char *op = jv_obj_get(c, "op")->u.s.s;
    JVal *lv = resolve_value(ctx, jv_obj_get(c, "left"), cur, e, ok);
    if (!*ok)
        return NULL;
    JVal *rv = resolve_value(ctx, jv_obj_get(c, "right"), cur, e, ok);
    if (!*ok)
        return NULL;
    int r;
    if (strcmp(op, "eq") == 0) {
        r = jv_equal(lv, rv);
    } else if (strcmp(op, "ne") == 0) {
        r = !jv_equal(lv, rv);
    } else if (strcmp(op, "gt") == 0) {
        r = jv_compare(lv, rv) > 0;
    } else if (strcmp(op, "gte") == 0) {
        r = jv_compare(lv, rv) >= 0;
    } else if (strcmp(op, "lt") == 0) {
        r = jv_compare(lv, rv) < 0;
    } else {
        r = jv_compare(lv, rv) <= 0;
    }
    return jv_bool(r);
}

static JVal *eval_condition(Ctx *ctx, JVal *c, int cur, Err *e, int *ok)
{
    const char *op = jv_obj_get(c, "op")->u.s.s;
    JVal *res = NULL;

    if (strcmp(op, "eq") == 0 || strcmp(op, "ne") == 0 ||
        strcmp(op, "gt") == 0 || strcmp(op, "gte") == 0 ||
        strcmp(op, "lt") == 0 || strcmp(op, "lte") == 0) {
        res = eval_comparison(ctx, c, cur, e, ok);
    } else if (strcmp(op, "all") == 0 || strcmp(op, "any") == 0) {
        int is_all = strcmp(op, "all") == 0;
        int acc = is_all ? 1 : 0;
        JVal *cons = jv_obj_get(c, "conditions");
        for (size_t i = 0; i < cons->u.a.n; i++) {
            JVal *sub = eval_condition(ctx, cons->u.a.v[i], cur, e, ok);
            if (!*ok)
                return NULL;
            if (!sub || sub->type != J_BOOL) {
                err_set(e, E_INVALID_CONDITION, ctx->steps[cur].id,
                        "condition did not evaluate to a boolean");
                *ok = 0;
                return NULL;
            }
            if (is_all)
                acc = acc && sub->u.b;
            else
                acc = acc || sub->u.b;
        }
        res = jv_bool(acc);
    } else if (strcmp(op, "not") == 0) {
        JVal *sub = eval_condition(ctx, jv_obj_get(c, "condition"), cur, e, ok);
        if (!*ok)
            return NULL;
        if (!sub || sub->type != J_BOOL) {
            err_set(e, E_INVALID_CONDITION, ctx->steps[cur].id,
                    "condition did not evaluate to a boolean");
            *ok = 0;
            return NULL;
        }
        res = jv_bool(!sub->u.b);
    } else {
        err_set(e, E_INVALID_CONDITION, ctx->steps[cur].id,
                "unknown condition operator '%s'", op);
        *ok = 0;
        return NULL;
    }
    if (res && res->type != J_BOOL) {
        err_set(e, E_INVALID_CONDITION, ctx->steps[cur].id,
                "condition did not evaluate to a boolean");
        *ok = 0;
        return NULL;
    }
    return res;
}

/* ------------------------------------------------------------------ */
/* argument helpers                                                    */
/* ------------------------------------------------------------------ */

static int arg_keys(JVal *args, const char *const *allowed, int n, Err *e)
{
    return check_keys(args, allowed, n, E_INVALID_ARG, NULL, e);
}

static int need(JVal *args, const char *name, JVal **out, Err *e)
{
    JVal *v = jv_obj_get(args, name);
    if (!v) {
        err_set(e, E_INVALID_ARG, NULL, "missing argument '%s'", name);
        return -1;
    }
    *out = v;
    return 0;
}

static int need_i64(JVal *args, const char *name, int64_t *out, Err *e)
{
    JVal *v;
    if (need(args, name, &v, e) < 0)
        return -1;
    if (get_i64(v, out) < 0) {
        err_set(e, E_INVALID_ARG, NULL,
                "argument '%s' must be a signed 64-bit integer", name);
        return -1;
    }
    return 0;
}

static int opt_i64(JVal *args, const char *name, int64_t *out, int *present, Err *e)
{
    JVal *v = jv_obj_get(args, name);
    if (!v) {
        *present = 0;
        return 0;
    }
    if (get_i64(v, out) < 0) {
        err_set(e, E_INVALID_ARG, NULL,
                "argument '%s' must be a signed 64-bit integer", name);
        return -1;
    }
    *present = 1;
    return 0;
}

static int need_str(JVal *args, const char *name, const JStr **out, Err *e)
{
    JVal *v;
    if (need(args, name, &v, e) < 0)
        return -1;
    if (v->type != J_STR) {
        err_set(e, E_INVALID_ARG, NULL, "argument '%s' must be a string", name);
        return -1;
    }
    *out = &v->u.s;
    return 0;
}

static int need_arr(JVal *args, const char *name, JVal **out, Err *e)
{
    JVal *v;
    if (need(args, name, &v, e) < 0)
        return -1;
    if (v->type != J_ARR) {
        err_set(e, E_INVALID_ARG, NULL, "argument '%s' must be an array", name);
        return -1;
    }
    *out = v;
    return 0;
}

static int need_obj(JVal *args, const char *name, JVal **out, Err *e)
{
    JVal *v;
    if (need(args, name, &v, e) < 0)
        return -1;
    if (v->type != J_OBJ) {
        err_set(e, E_INVALID_ARG, NULL, "argument '%s' must be an object", name);
        return -1;
    }
    *out = v;
    return 0;
}

/* ------------------------------------------------------------------ */
/* i64 arithmetic                                                      */
/* ------------------------------------------------------------------ */

static int do_arith(const char *op, int64_t a, int64_t b, int64_t *r, Err *e)
{
    if (strcmp(op, "add") == 0) {
        if (__builtin_add_overflow(a, b, r)) {
            err_set(e, E_ARITHMETIC_OVERFLOW, NULL, "addition overflow");
            return -1;
        }
        return 0;
    }
    if (strcmp(op, "subtract") == 0) {
        if (__builtin_sub_overflow(a, b, r)) {
            err_set(e, E_ARITHMETIC_OVERFLOW, NULL, "subtraction overflow");
            return -1;
        }
        return 0;
    }
    if (strcmp(op, "multiply") == 0) {
        if (__builtin_mul_overflow(a, b, r)) {
            err_set(e, E_ARITHMETIC_OVERFLOW, NULL, "multiplication overflow");
            return -1;
        }
        return 0;
    }
    if (strcmp(op, "divide") == 0) {
        if (b == 0) {
            err_set(e, E_DIVISION_BY_ZERO, NULL, "division by zero");
            return -1;
        }
        if (a == INT64_MIN && b == -1) {
            err_set(e, E_ARITHMETIC_OVERFLOW, NULL, "division overflow");
            return -1;
        }
        *r = a / b;
        return 0;
    }
    if (strcmp(op, "mod") == 0) {
        if (b == 0) {
            err_set(e, E_DIVISION_BY_ZERO, NULL, "modulo by zero");
            return -1;
        }
        if (a == INT64_MIN && b == -1) {
            *r = 0;
            return 0;
        }
        *r = a % b;
        return 0;
    }
    err_set(e, E_INVALID_ARG, NULL, "unknown arithmetic operator '%s'", op);
    return -1;
}

/* ------------------------------------------------------------------ */
/* transforms / predicates                                             */
/* ------------------------------------------------------------------ */

static const char *const T_OP_KEYS[] = { "type", "op", "args" };
static const char *const T_VALUE_KEYS[] = { "type", "value" };
static const char *const T_TYPE_KEYS[] = { "type" };

static int apply_transform(Ctx *ctx, int cur, JVal *tr, JVal *elem,
                           JVal **out, Err *e)
{
    if (!tr || tr->type != J_OBJ) {
        err_set(e, E_INVALID_ARG, NULL, "transform must be an object");
        return -1;
    }
    JVal *tv = jv_obj_get(tr, "type");
    if (!tv || tv->type != J_STR) {
        err_set(e, E_INVALID_ARG, NULL, "transform requires a string 'type'");
        return -1;
    }
    const char *type = tv->u.s.s;

    if (strcmp(type, "op") == 0) {
        if (arg_keys(tr, T_OP_KEYS, 3, e) < 0)
            return -1;
        JVal *opv = jv_obj_get(tr, "op");
        JVal *av = jv_obj_get(tr, "args");
        if (!opv || opv->type != J_STR || !av || av->type != J_OBJ) {
            err_set(e, E_INVALID_ARG, NULL,
                    "transform 'op' requires string 'op' and object 'args'");
            return -1;
        }
        const char *op = opv->u.s.s;
        if (strcmp(op, "add") != 0 && strcmp(op, "subtract") != 0 &&
            strcmp(op, "multiply") != 0 && strcmp(op, "divide") != 0 &&
            strcmp(op, "mod") != 0) {
            err_set(e, E_INVALID_ARG, NULL,
                    "transform op '%s' is not supported", op);
            return -1;
        }
        static const char *const ARG_KEYS[] = { "a", "b" };
        if (arg_keys(av, ARG_KEYS, 2, e) < 0)
            return -1;
        JVal *a = jv_obj_get(av, "a");
        JVal *b = jv_obj_get(av, "b");
        if (!a || !b) {
            err_set(e, E_INVALID_ARG, NULL,
                    "transform op requires 'a' and 'b' arguments");
            return -1;
        }
        int64_t x, y;
        int ok = 1;
        if (is_self(a)) {
            if (get_i64(elem, &x) < 0) {
                err_set(e, E_INVALID_ARG, NULL,
                        "map element must be an i64 for numeric transforms");
                return -1;
            }
        } else {
            JVal *ra = resolve_value(ctx, a, cur, e, &ok);
            if (!ok)
                return -1;
            if (get_i64(ra, &x) < 0) {
                err_set(e, E_INVALID_ARG, NULL,
                        "transform operand 'a' must be an i64");
                return -1;
            }
        }
        if (is_self(b)) {
            if (get_i64(elem, &y) < 0) {
                err_set(e, E_INVALID_ARG, NULL,
                        "map element must be an i64 for numeric transforms");
                return -1;
            }
        } else {
            JVal *rb = resolve_value(ctx, b, cur, e, &ok);
            if (!ok)
                return -1;
            if (get_i64(rb, &y) < 0) {
                err_set(e, E_INVALID_ARG, NULL,
                        "transform operand 'b' must be an i64");
                return -1;
            }
        }
        int64_t r;
        if (do_arith(op, x, y, &r, e) < 0)
            return -1;
        *out = jv_int(r);
        return 0;
    }

    if (strcmp(type, "constant") == 0) {
        if (arg_keys(tr, T_VALUE_KEYS, 2, e) < 0)
            return -1;
        JVal *v;
        if (need(tr, "value", &v, e) < 0)
            return -1;
        *out = v;
        return 0;
    }

    if (strcmp(type, "add") == 0 || strcmp(type, "subtract") == 0 ||
        strcmp(type, "multiply") == 0 || strcmp(type, "divide") == 0 ||
        strcmp(type, "mod") == 0) {
        if (arg_keys(tr, T_VALUE_KEYS, 2, e) < 0)
            return -1;
        int64_t y, x;
        if (need_i64(tr, "value", &y, e) < 0)
            return -1;
        if (get_i64(elem, &x) < 0) {
            err_set(e, E_INVALID_ARG, NULL,
                    "map element must be an i64 for numeric transforms");
            return -1;
        }
        int64_t r;
        if (do_arith(type, x, y, &r, e) < 0)
            return -1;
        *out = jv_int(r);
        return 0;
    }

    if (strcmp(type, "negate") == 0 || strcmp(type, "abs") == 0) {
        if (arg_keys(tr, T_TYPE_KEYS, 1, e) < 0)
            return -1;
        int64_t x;
        if (get_i64(elem, &x) < 0) {
            err_set(e, E_INVALID_ARG, NULL,
                    "map element must be an i64 for '%s'", type);
            return -1;
        }
        if (x == INT64_MIN) {
            err_set(e, E_ARITHMETIC_OVERFLOW, NULL, "%s overflow", type);
            return -1;
        }
        *out = jv_int(strcmp(type, "negate") == 0 ? -x : (x < 0 ? -x : x));
        return 0;
    }

    if (strcmp(type, "stringify") == 0) {
        if (arg_keys(tr, T_TYPE_KEYS, 1, e) < 0)
            return -1;
        size_t len = 0;
        char *s = jv_dump(elem, &len);
        *out = jv_str(s, len);
        free(s);
        return 0;
    }

    if (strcmp(type, "type") == 0) {
        if (arg_keys(tr, T_TYPE_KEYS, 1, e) < 0)
            return -1;
        *out = mkcstr(jv_type_name(elem));
        return 0;
    }

    err_set(e, E_INVALID_ARG, NULL, "unknown transform type '%s'", type);
    return -1;
}

static const char *const P_TYPE_KEYS[] = { "type" };
static const char *const P_VALUE_KEYS[] = { "type", "value" };
static const char *const P_TYPEIS_KEYS[] = {
    "type", "value", "type_name", "of", "target"
};

static int is_type_name(const char *s)
{
    return strcmp(s, "null") == 0 || strcmp(s, "boolean") == 0 ||
           strcmp(s, "number") == 0 || strcmp(s, "string") == 0 ||
           strcmp(s, "array") == 0 || strcmp(s, "object") == 0;
}

/* Determine the target type for a type_is predicate, coping with the
 * ambiguous documented shape (which would use the key "type" twice). */
static JStr *type_is_target(JVal *pred, const char *disc)
{
    static const char *const ALT[] = { "value", "type_name", "of", "target" };
    if (strcmp(disc, "type_is") == 0) {
        for (size_t i = 0; i < sizeof(ALT) / sizeof(ALT[0]); i++) {
            JVal *v = jv_obj_get(pred, ALT[i]);
            if (v && v->type == J_STR)
                return &v->u.s;
        }
        /* second "type" entry, if the raw document used a duplicate key */
        int seen_first = 0;
        for (size_t i = 0; i < pred->u.o.n; i++) {
            if (jstr_eq_c(&pred->u.o.k[i], "type")) {
                if (!seen_first) {
                    seen_first = 1;
                    continue;
                }
                if (pred->u.o.v[i]->type == J_STR)
                    return &pred->u.o.v[i]->u.s;
            }
        }
        return NULL;
    }
    /* A collapsed duplicate key leaves {"type": "<type name>"}. */
    if (is_type_name(disc) && pred->u.o.n == 1) {
        JVal *v = jv_obj_get(pred, "type");
        return (v && v->type == J_STR) ? &v->u.s : NULL;
    }
    return NULL;
}

/* Validate a transform's declared shape once, before iterating elements. */
static int check_transform_shape(JVal *tr, Err *e)
{
    if (!tr || tr->type != J_OBJ) {
        err_set(e, E_INVALID_ARG, NULL, "transform must be an object");
        return -1;
    }
    JVal *tv = jv_obj_get(tr, "type");
    if (!tv || tv->type != J_STR) {
        err_set(e, E_INVALID_ARG, NULL, "transform requires a string 'type'");
        return -1;
    }
    const char *type = tv->u.s.s;
    if (strcmp(type, "op") == 0) {
        if (arg_keys(tr, T_OP_KEYS, 3, e) < 0)
            return -1;
        JVal *opv = jv_obj_get(tr, "op");
        JVal *av = jv_obj_get(tr, "args");
        if (!opv || opv->type != J_STR || !av || av->type != J_OBJ) {
            err_set(e, E_INVALID_ARG, NULL,
                    "transform 'op' requires string 'op' and object 'args'");
            return -1;
        }
        const char *op = opv->u.s.s;
        if (strcmp(op, "add") != 0 && strcmp(op, "subtract") != 0 &&
            strcmp(op, "multiply") != 0 && strcmp(op, "divide") != 0 &&
            strcmp(op, "mod") != 0) {
            err_set(e, E_INVALID_ARG, NULL,
                    "transform op '%s' is not supported", op);
            return -1;
        }
        static const char *const ARG_KEYS[] = { "a", "b" };
        if (arg_keys(av, ARG_KEYS, 2, e) < 0)
            return -1;
        JVal *a = jv_obj_get(av, "a");
        JVal *b = jv_obj_get(av, "b");
        if (!a || !b) {
            err_set(e, E_INVALID_ARG, NULL,
                    "transform op requires 'a' and 'b' arguments");
            return -1;
        }
        if ((!is_self(a) && get_i64(a, &(int64_t){0}) < 0) ||
            (!is_self(b) && get_i64(b, &(int64_t){0}) < 0)) {
            err_set(e, E_INVALID_ARG, NULL,
                    "transform operands must be i64 or {$self:true}");
            return -1;
        }
        return 0;
    }
    if (strcmp(type, "constant") == 0) {
        if (arg_keys(tr, T_VALUE_KEYS, 2, e) < 0)
            return -1;
        if (!jv_obj_has(tr, "value")) {
            err_set(e, E_INVALID_ARG, NULL,
                    "transform 'constant' requires a 'value'");
            return -1;
        }
        return 0;
    }
    if (strcmp(type, "add") == 0 || strcmp(type, "subtract") == 0 ||
        strcmp(type, "multiply") == 0 || strcmp(type, "divide") == 0 ||
        strcmp(type, "mod") == 0) {
        if (arg_keys(tr, T_VALUE_KEYS, 2, e) < 0)
            return -1;
        int64_t dummy;
        if (need_i64(tr, "value", &dummy, e) < 0)
            return -1;
        return 0;
    }
    if (strcmp(type, "negate") == 0 || strcmp(type, "abs") == 0 ||
        strcmp(type, "stringify") == 0 || strcmp(type, "type") == 0) {
        return arg_keys(tr, T_TYPE_KEYS, 1, e);
    }
    err_set(e, E_INVALID_ARG, NULL, "unknown transform type '%s'", type);
    return -1;
}

/* Validate a predicate's declared shape once, before iterating elements. */
static int check_predicate_shape(JVal *pred, Err *e)
{
    if (!pred || pred->type != J_OBJ) {
        err_set(e, E_INVALID_ARG, NULL, "predicate must be an object");
        return -1;
    }
    JVal *tv = jv_obj_get(pred, "type");
    if (!tv || tv->type != J_STR) {
        err_set(e, E_INVALID_ARG, NULL, "predicate requires a string 'type'");
        return -1;
    }
    const char *type = tv->u.s.s;
    if (strcmp(type, "even") == 0 || strcmp(type, "odd") == 0 ||
        strcmp(type, "truthy") == 0 || strcmp(type, "falsy") == 0)
        return arg_keys(pred, P_TYPE_KEYS, 1, e);
    if (strcmp(type, "gt") == 0 || strcmp(type, "gte") == 0 ||
        strcmp(type, "lt") == 0 || strcmp(type, "lte") == 0 ||
        strcmp(type, "eq") == 0 || strcmp(type, "ne") == 0) {
        if (arg_keys(pred, P_VALUE_KEYS, 2, e) < 0)
            return -1;
        if (!jv_obj_has(pred, "value")) {
            err_set(e, E_INVALID_ARG, NULL,
                    "predicate '%s' requires a 'value'", type);
            return -1;
        }
        return 0;
    }
    if (strcmp(type, "type_is") == 0) {
        if (arg_keys(pred, P_TYPEIS_KEYS, 5, e) < 0)
            return -1;
        if (!type_is_target(pred, type)) {
            err_set(e, E_INVALID_ARG, NULL,
                    "predicate 'type_is' requires a target type");
            return -1;
        }
        return 0;
    }
    if (is_type_name(type)) {
        if (arg_keys(pred, P_TYPE_KEYS, 1, e) < 0)
            return -1;
        if (!type_is_target(pred, type)) {
            err_set(e, E_INVALID_ARG, NULL,
                    "predicate requires a target type");
            return -1;
        }
        return 0;
    }
    if (strcmp(type, "string_contains") == 0) {
        if (arg_keys(pred, P_VALUE_KEYS, 2, e) < 0)
            return -1;
        JVal *v = jv_obj_get(pred, "value");
        if (!v || v->type != J_STR) {
            err_set(e, E_INVALID_ARG, NULL,
                    "predicate 'string_contains' requires a string 'value'");
            return -1;
        }
        return 0;
    }
    err_set(e, E_INVALID_ARG, NULL, "unknown predicate type '%s'", type);
    return -1;
}

static int apply_predicate(JVal *pred, JVal *elem, Err *e)
{
    if (!pred || pred->type != J_OBJ) {
        err_set(e, E_INVALID_ARG, NULL, "predicate must be an object");
        return -1;
    }
    JVal *tv = jv_obj_get(pred, "type");
    if (!tv || tv->type != J_STR) {
        err_set(e, E_INVALID_ARG, NULL, "predicate requires a string 'type'");
        return -1;
    }
    const char *type = tv->u.s.s;

    if (strcmp(type, "even") == 0 || strcmp(type, "odd") == 0) {
        if (arg_keys(pred, P_TYPE_KEYS, 1, e) < 0)
            return -1;
        int64_t x;
        if (get_i64(elem, &x) < 0) {
            err_set(e, E_INVALID_ARG, NULL,
                    "filter element must be an i64 for '%s'", type);
            return -1;
        }
        int even = (x % 2) == 0;
        return strcmp(type, "even") == 0 ? even : !even;
    }

    if (strcmp(type, "gt") == 0 || strcmp(type, "gte") == 0 ||
        strcmp(type, "lt") == 0 || strcmp(type, "lte") == 0 ||
        strcmp(type, "eq") == 0 || strcmp(type, "ne") == 0) {
        if (arg_keys(pred, P_VALUE_KEYS, 2, e) < 0)
            return -1;
        JVal *val;
        if (need(pred, "value", &val, e) < 0)
            return -1;
        int c = jv_compare(elem, val);
        if (strcmp(type, "gt") == 0)
            return c > 0;
        if (strcmp(type, "gte") == 0)
            return c >= 0;
        if (strcmp(type, "lt") == 0)
            return c < 0;
        if (strcmp(type, "lte") == 0)
            return c <= 0;
        if (strcmp(type, "eq") == 0)
            return jv_equal(elem, val);
        return !jv_equal(elem, val);
    }

    if (strcmp(type, "truthy") == 0 || strcmp(type, "falsy") == 0) {
        if (arg_keys(pred, P_TYPE_KEYS, 1, e) < 0)
            return -1;
        int t = jv_truthy(elem);
        return strcmp(type, "truthy") == 0 ? t : !t;
    }

    if (strcmp(type, "type_is") == 0 || is_type_name(type)) {
        if (strcmp(type, "type_is") == 0) {
            if (arg_keys(pred, P_TYPEIS_KEYS, 5, e) < 0)
                return -1;
        } else {
            if (arg_keys(pred, P_TYPE_KEYS, 1, e) < 0)
                return -1;
        }
        JStr *target = type_is_target(pred, type);
        if (!target) {
            err_set(e, E_INVALID_ARG, NULL,
                    "predicate 'type_is' requires a target type");
            return -1;
        }
        size_t n = strlen(jv_type_name(elem));
        const char *tn = jv_type_name(elem);
        return target->len == n && (n == 0 || memcmp(target->s, tn, n) == 0);
    }

    if (strcmp(type, "string_contains") == 0) {
        if (arg_keys(pred, P_VALUE_KEYS, 2, e) < 0)
            return -1;
        const JStr *needle;
        if (need_str(pred, "value", &needle, e) < 0)
            return -1;
        if (elem->type != J_STR) {
            err_set(e, E_INVALID_ARG, NULL,
                    "filter element must be a string for 'string_contains'");
            return -1;
        }
        const JStr *hay = &elem->u.s;
        if (needle->len == 0)
            return 1;
        if (needle->len > hay->len)
            return 0;
        for (size_t i = 0; i + needle->len <= hay->len; i++) {
            if (memcmp(hay->s + i, needle->s, needle->len) == 0)
                return 1;
        }
        return 0;
    }

    err_set(e, E_INVALID_ARG, NULL, "unknown predicate type '%s'", type);
    return -1;
}

/* ------------------------------------------------------------------ */
/* operations                                                          */
/* ------------------------------------------------------------------ */

static int cmp_jstr_local(const void *pa, const void *pb)
{
    const JStr *a = pa, *b = pb;
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

static void stable_sort(JVal **a, JVal **tmp, size_t n)
{
    if (n < 2)
        return;
    size_t m = n / 2;
    stable_sort(a, tmp, m);
    stable_sort(a + m, tmp, n - m);
    size_t i = 0, j = m, k = 0;
    while (i < m && j < n) {
        if (jv_compare(a[j], a[i]) < 0)
            tmp[k++] = a[j++];
        else
            tmp[k++] = a[i++];
    }
    while (i < m)
        tmp[k++] = a[i++];
    while (j < n)
        tmp[k++] = a[j++];
    memcpy(a, tmp, n * sizeof(JVal *));
}

/* sort an array of JStr structs */
static void sort_jstrs(JStr *v, size_t n)
{
    if (n > 1)
        qsort(v, n, sizeof(JStr), cmp_jstr_local);
}

/* Build a slice of an array of JStr keys sorted and deduped. */
static size_t collect_sorted_keys(JVal *keys_arr, JStr **out, Err *e)
{
    size_t n = keys_arr->u.a.n;
    JStr *arr = malloc((n ? n : 1) * sizeof(JStr));
    if (!arr)
        jv_oom();
    for (size_t i = 0; i < n; i++) {
        if (keys_arr->u.a.v[i]->type != J_STR) {
            free(arr);
            err_set(e, E_INVALID_ARG, NULL,
                    "argument 'keys' must contain only strings");
            return 0;
        }
        arr[i] = keys_arr->u.a.v[i]->u.s;
    }
    sort_jstrs(arr, n);
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        if (w == 0 || cmp_jstr_local(&arr[i], &arr[w - 1]) != 0)
            arr[w++] = arr[i];
    }
    *out = arr;
    return w;
}

static int run_op(Ctx *ctx, int cur, const char *op, JVal *args,
                  JVal **out, Err *e)
{
    Step *st = &ctx->steps[cur];

    /* ---------------- math ---------------- */
    if (strcmp(op, "add") == 0 || strcmp(op, "subtract") == 0 ||
        strcmp(op, "multiply") == 0 || strcmp(op, "divide") == 0 ||
        strcmp(op, "mod") == 0) {
        static const char *const K[] = { "a", "b" };
        if (arg_keys(args, K, 2, e) < 0)
            return -1;
        int64_t a, b, r;
        if (need_i64(args, "a", &a, e) < 0)
            return -1;
        if (need_i64(args, "b", &b, e) < 0)
            return -1;
        if (do_arith(op, a, b, &r, e) < 0)
            return -1;
        *out = jv_int(r);
        return 0;
    }

    if (strcmp(op, "sum") == 0 || strcmp(op, "min") == 0 ||
        strcmp(op, "max") == 0) {
        static const char *const K[] = { "input" };
        if (arg_keys(args, K, 1, e) < 0)
            return -1;
        JVal *in;
        if (need_arr(args, "input", &in, e) < 0)
            return -1;
        if (in->u.a.n == 0 && strcmp(op, "sum") != 0) {
            err_set(e, E_EMPTY_ARRAY, NULL, "%s of empty array", op);
            return -1;
        }
        int64_t acc = 0;
        int first = 1;
        for (size_t i = 0; i < in->u.a.n; i++) {
            int64_t x;
            if (get_i64(in->u.a.v[i], &x) < 0) {
                err_set(e, E_INVALID_ARG, NULL,
                        "array elements must be signed 64-bit integers");
                return -1;
            }
            if (strcmp(op, "sum") == 0) {
                int64_t r;
                if (__builtin_add_overflow(acc, x, &r)) {
                    err_set(e, E_ARITHMETIC_OVERFLOW, NULL, "sum overflow");
                    return -1;
                }
                acc = r;
            } else if (first) {
                acc = x;
                first = 0;
            } else if (strcmp(op, "min") == 0) {
                if (x < acc)
                    acc = x;
            } else {
                if (x > acc)
                    acc = x;
            }
        }
        *out = jv_int(acc);
        return 0;
    }

    if (strcmp(op, "range") == 0) {
        static const char *const K[] = { "start", "end" };
        if (arg_keys(args, K, 2, e) < 0)
            return -1;
        int64_t start, end;
        if (need_i64(args, "start", &start, e) < 0)
            return -1;
        if (need_i64(args, "end", &end, e) < 0)
            return -1;
        JVal *arr = jv_arr();
        if (start <= end) {
            unsigned long long count =
                (unsigned long long)end - (unsigned long long)start + 1ULL;
            if (count == 0 || count > 100000000ULL) {
                err_set(e, E_INVALID_ARG, NULL,
                        "range is too large to materialize");
                return -1;
            }
            for (unsigned long long k = 0; k < count; k++)
                jv_arr_push(arr, jv_int((int64_t)(start + (int64_t)k)));
        }
        *out = arr;
        return 0;
    }

    /* ---------------- strings ---------------- */
    if (strcmp(op, "concat") == 0) {
        static const char *const K[] = { "a", "b" };
        if (arg_keys(args, K, 2, e) < 0)
            return -1;
        const JStr *a, *b;
        if (need_str(args, "a", &a, e) < 0)
            return -1;
        if (need_str(args, "b", &b, e) < 0)
            return -1;
        SB sb = {0};
        sb_put(&sb, a->s, a->len);
        sb_put(&sb, b->s, b->len);
        size_t len;
        char *s = sb_take(&sb, &len);
        *out = jv_str(s, len);
        free(s);
        return 0;
    }

    if (strcmp(op, "split") == 0) {
        static const char *const K[] = { "input", "sep" };
        if (arg_keys(args, K, 2, e) < 0)
            return -1;
        const JStr *in, *sep;
        if (need_str(args, "input", &in, e) < 0)
            return -1;
        if (need_str(args, "sep", &sep, e) < 0)
            return -1;
        JVal *arr = jv_arr();
        if (sep->len == 0) {
            size_t i = 0;
            while (i < in->len) {
                size_t n = cp_len((const unsigned char *)in->s + i,
                                  in->len - i);
                jv_arr_push(arr, jv_str(in->s + i, n));
                i += n;
            }
        } else {
            size_t start = 0, i = 0;
            while (i + sep->len <= in->len) {
                if (memcmp(in->s + i, sep->s, sep->len) == 0) {
                    jv_arr_push(arr, jv_str(in->s + start, i - start));
                    i += sep->len;
                    start = i;
                } else {
                    i++;
                }
            }
            jv_arr_push(arr, jv_str(in->s + start, in->len - start));
        }
        *out = arr;
        return 0;
    }

    if (strcmp(op, "join") == 0) {
        static const char *const K[] = { "input", "sep" };
        if (arg_keys(args, K, 2, e) < 0)
            return -1;
        JVal *in;
        const JStr *sep;
        if (need_arr(args, "input", &in, e) < 0)
            return -1;
        if (need_str(args, "sep", &sep, e) < 0)
            return -1;
        for (size_t i = 0; i < in->u.a.n; i++) {
            if (in->u.a.v[i]->type != J_STR) {
                err_set(e, E_INVALID_ARG, NULL,
                        "join input must be an array of strings");
                return -1;
            }
        }
        SB sb = {0};
        for (size_t i = 0; i < in->u.a.n; i++) {
            if (i)
                sb_put(&sb, sep->s, sep->len);
            sb_put(&sb, in->u.a.v[i]->u.s.s, in->u.a.v[i]->u.s.len);
        }
        size_t len;
        char *s = sb_take(&sb, &len);
        *out = jv_str(s, len);
        free(s);
        return 0;
    }

    if (strcmp(op, "uppercase") == 0 || strcmp(op, "lowercase") == 0) {
        static const char *const K[] = { "input" };
        if (arg_keys(args, K, 1, e) < 0)
            return -1;
        const JStr *in;
        if (need_str(args, "input", &in, e) < 0)
            return -1;
        char *buf = malloc(in->len + 1);
        if (!buf)
            jv_oom();
        for (size_t i = 0; i < in->len; i++) {
            unsigned char c = (unsigned char)in->s[i];
            if (strcmp(op, "uppercase") == 0) {
                if (c >= 'a' && c <= 'z')
                    c = (unsigned char)(c - 'a' + 'A');
            } else {
                if (c >= 'A' && c <= 'Z')
                    c = (unsigned char)(c - 'A' + 'a');
            }
            buf[i] = (char)c;
        }
        *out = jv_str(buf, in->len);
        free(buf);
        return 0;
    }

    if (strcmp(op, "trim") == 0) {
        static const char *const K[] = { "input" };
        if (arg_keys(args, K, 1, e) < 0)
            return -1;
        const JStr *in;
        if (need_str(args, "input", &in, e) < 0)
            return -1;
        size_t a = 0, b = in->len;
        while (a < b && (in->s[a] == ' ' || in->s[a] == '\t' ||
                         in->s[a] == '\n' || in->s[a] == '\r' ||
                         in->s[a] == '\f' || in->s[a] == '\v'))
            a++;
        while (b > a && (in->s[b - 1] == ' ' || in->s[b - 1] == '\t' ||
                         in->s[b - 1] == '\n' || in->s[b - 1] == '\r' ||
                         in->s[b - 1] == '\f' || in->s[b - 1] == '\v'))
            b--;
        *out = jv_str(in->s + a, b - a);
        return 0;
    }

    if (strcmp(op, "replace") == 0) {
        static const char *const K[] = { "input", "search", "replace" };
        if (arg_keys(args, K, 3, e) < 0)
            return -1;
        const JStr *in, *search, *rep;
        if (need_str(args, "input", &in, e) < 0)
            return -1;
        if (need_str(args, "search", &search, e) < 0)
            return -1;
        if (need_str(args, "replace", &rep, e) < 0)
            return -1;
        SB sb = {0};
        if (search->len == 0) {
            size_t i = 0;
            while (i < in->len) {
                size_t n = cp_len((const unsigned char *)in->s + i,
                                  in->len - i);
                sb_put(&sb, rep->s, rep->len);
                sb_put(&sb, in->s + i, n);
                i += n;
            }
            sb_put(&sb, rep->s, rep->len);
        } else {
            size_t i = 0, start = 0;
            while (i + search->len <= in->len) {
                if (memcmp(in->s + i, search->s, search->len) == 0) {
                    sb_put(&sb, in->s + start, i - start);
                    sb_put(&sb, rep->s, rep->len);
                    i += search->len;
                    start = i;
                } else {
                    i++;
                }
            }
            sb_put(&sb, in->s + start, in->len - start);
        }
        size_t len;
        char *s = sb_take(&sb, &len);
        *out = jv_str(s, len);
        free(s);
        return 0;
    }

    if (strcmp(op, "length") == 0) {
        static const char *const K[] = { "input" };
        if (arg_keys(args, K, 1, e) < 0)
            return -1;
        const JStr *in;
        if (need_str(args, "input", &in, e) < 0)
            return -1;
        int64_t n = 0;
        for (size_t i = 0; i < in->len; i++) {
            if (((unsigned char)in->s[i] & 0xC0) != 0x80)
                n++;
        }
        *out = jv_int(n);
        return 0;
    }

    /* ---------------- arrays ---------------- */
    if (strcmp(op, "map") == 0) {
        static const char *const K[] = { "input", "transform" };
        if (arg_keys(args, K, 2, e) < 0)
            return -1;
        JVal *in;
        if (need_arr(args, "input", &in, e) < 0)
            return -1;
        JVal *tr = jv_obj_get(args, "transform");
        if (!tr) {
            err_set(e, E_INVALID_ARG, NULL, "missing argument 'transform'");
            return -1;
        }
        if (check_transform_shape(tr, e) < 0)
            return -1;
        JVal *arr = jv_arr();
        for (size_t i = 0; i < in->u.a.n; i++) {
            JVal *r = NULL;
            if (apply_transform(ctx, cur, tr, in->u.a.v[i], &r, e) < 0)
                return -1;
            jv_arr_push(arr, r);
        }
        *out = arr;
        return 0;
    }

    if (strcmp(op, "filter") == 0) {
        static const char *const K[] = { "input", "predicate" };
        if (arg_keys(args, K, 2, e) < 0)
            return -1;
        JVal *in;
        if (need_arr(args, "input", &in, e) < 0)
            return -1;
        JVal *pred = jv_obj_get(args, "predicate");
        if (!pred) {
            err_set(e, E_INVALID_ARG, NULL, "missing argument 'predicate'");
            return -1;
        }
        if (check_predicate_shape(pred, e) < 0)
            return -1;
        JVal *arr = jv_arr();
        for (size_t i = 0; i < in->u.a.n; i++) {
            int keep = apply_predicate(pred, in->u.a.v[i], e);
            if (keep < 0)
                return -1;
            if (keep)
                jv_arr_push(arr, in->u.a.v[i]);
        }
        *out = arr;
        return 0;
    }

    if (strcmp(op, "reduce") == 0) {
        static const char *const K[] = { "input", "op", "initial" };
        if (arg_keys(args, K, 3, e) < 0)
            return -1;
        JVal *in;
        if (need_arr(args, "input", &in, e) < 0)
            return -1;
        const JStr *rop;
        if (need_str(args, "op", &rop, e) < 0)
            return -1;
        if (!jstr_eq_c(rop, "add") &&
            !jstr_eq_c(rop, "subtract") &&
            !jstr_eq_c(rop, "multiply")) {
            err_set(e, E_INVALID_ARG, NULL,
                    "reduce 'op' must be add, subtract or multiply");
            return -1;
        }
        int64_t acc;
        int present = 0;
        if (opt_i64(args, "initial", &acc, &present, e) < 0)
            return -1;
        if (!present)
            acc = jstr_eq_c(rop, "multiply") ? 1 : 0;
        for (size_t i = 0; i < in->u.a.n; i++) {
            int64_t x;
            if (get_i64(in->u.a.v[i], &x) < 0) {
                err_set(e, E_INVALID_ARG, NULL,
                        "reduce input must be an array of i64");
                return -1;
            }
            int64_t r;
            if (do_arith(rop->s, acc, x, &r, e) < 0)
                return -1;
            acc = r;
        }
        *out = jv_int(acc);
        return 0;
    }

    if (strcmp(op, "sort") == 0) {
        static const char *const K[] = { "input" };
        if (arg_keys(args, K, 1, e) < 0)
            return -1;
        JVal *in;
        if (need_arr(args, "input", &in, e) < 0)
            return -1;
        size_t n = in->u.a.n;
        JVal **copy = malloc((n ? n : 1) * sizeof(JVal *));
        JVal **tmp = malloc((n ? n : 1) * sizeof(JVal *));
        if (!copy || !tmp)
            jv_oom();
        memcpy(copy, in->u.a.v, n * sizeof(JVal *));
        stable_sort(copy, tmp, n);
        JVal *arr = jv_arr();
        for (size_t i = 0; i < n; i++)
            jv_arr_push(arr, copy[i]);
        free(copy);
        free(tmp);
        *out = arr;
        return 0;
    }

    if (strcmp(op, "reverse") == 0) {
        static const char *const K[] = { "input" };
        if (arg_keys(args, K, 1, e) < 0)
            return -1;
        JVal *in;
        if (need_arr(args, "input", &in, e) < 0)
            return -1;
        JVal *arr = jv_arr();
        for (size_t i = in->u.a.n; i > 0; i--)
            jv_arr_push(arr, in->u.a.v[i - 1]);
        *out = arr;
        return 0;
    }

    if (strcmp(op, "unique") == 0) {
        static const char *const K[] = { "input" };
        if (arg_keys(args, K, 1, e) < 0)
            return -1;
        JVal *in;
        if (need_arr(args, "input", &in, e) < 0)
            return -1;
        JVal *arr = jv_arr();
        for (size_t i = 0; i < in->u.a.n; i++) {
            int dup = 0;
            for (size_t j = 0; j < arr->u.a.n; j++) {
                if (jv_equal(arr->u.a.v[j], in->u.a.v[i])) {
                    dup = 1;
                    break;
                }
            }
            if (!dup)
                jv_arr_push(arr, in->u.a.v[i]);
        }
        *out = arr;
        return 0;
    }

    if (strcmp(op, "slice") == 0) {
        static const char *const K[] = { "input", "start", "end" };
        if (arg_keys(args, K, 3, e) < 0)
            return -1;
        JVal *in;
        if (need_arr(args, "input", &in, e) < 0)
            return -1;
        int64_t start, end;
        int has_end = 0;
        if (need_i64(args, "start", &start, e) < 0)
            return -1;
        if (opt_i64(args, "end", &end, &has_end, e) < 0)
            return -1;
        int64_t n = (int64_t)in->u.a.n;
        if (!has_end)
            end = n;
        if (start < 0) {
            start = (start < -n) ? 0 : n + start;
        } else if (start > n) {
            start = n;
        }
        if (end < 0) {
            end = (end < -n) ? 0 : n + end;
        } else if (end > n) {
            end = n;
        }
        if (end < start)
            end = start;
        JVal *arr = jv_arr();
        for (int64_t i = start; i < end; i++)
            jv_arr_push(arr, in->u.a.v[(size_t)i]);
        *out = arr;
        return 0;
    }

    if (strcmp(op, "flatten") == 0) {
        static const char *const K[] = { "input" };
        if (arg_keys(args, K, 1, e) < 0)
            return -1;
        JVal *in;
        if (need_arr(args, "input", &in, e) < 0)
            return -1;
        JVal *arr = jv_arr();
        for (size_t i = 0; i < in->u.a.n; i++) {
            JVal *el = in->u.a.v[i];
            if (el->type == J_ARR) {
                for (size_t j = 0; j < el->u.a.n; j++)
                    jv_arr_push(arr, el->u.a.v[j]);
            } else {
                jv_arr_push(arr, el);
            }
        }
        *out = arr;
        return 0;
    }

    /* ---------------- objects ---------------- */
    if (strcmp(op, "get") == 0) {
        static const char *const K[] = { "input", "key" };
        if (arg_keys(args, K, 2, e) < 0)
            return -1;
        JVal *in;
        const JStr *key;
        if (need_obj(args, "input", &in, e) < 0)
            return -1;
        if (need_str(args, "key", &key, e) < 0)
            return -1;
        JVal *v = jv_obj_get_len(in, key->s, key->len);
        *out = v ? v : jv_null();
        return 0;
    }

    if (strcmp(op, "set") == 0) {
        static const char *const K[] = { "input", "key", "value" };
        if (arg_keys(args, K, 3, e) < 0)
            return -1;
        JVal *in;
        const JStr *key;
        JVal *val;
        if (need_obj(args, "input", &in, e) < 0)
            return -1;
        if (need_str(args, "key", &key, e) < 0)
            return -1;
        if (need(args, "value", &val, e) < 0)
            return -1;
        JVal *obj = jv_obj();
        for (size_t i = 0; i < in->u.o.n; i++)
            jv_obj_append(obj, in->u.o.k[i].s, in->u.o.k[i].len,
                          in->u.o.v[i]);
        jv_obj_set(obj, key->s, key->len, val);
        *out = obj;
        return 0;
    }

    if (strcmp(op, "keys") == 0 || strcmp(op, "values") == 0) {
        static const char *const K[] = { "input" };
        if (arg_keys(args, K, 1, e) < 0)
            return -1;
        JVal *in;
        if (need_obj(args, "input", &in, e) < 0)
            return -1;
        JStr *keys = malloc((in->u.o.n ? in->u.o.n : 1) * sizeof(JStr));
        if (!keys)
            jv_oom();
        memcpy(keys, in->u.o.k, in->u.o.n * sizeof(JStr));
        sort_jstrs(keys, in->u.o.n);
        JVal *arr = jv_arr();
        for (size_t i = 0; i < in->u.o.n; i++) {
            if (strcmp(op, "keys") == 0) {
                jv_arr_push(arr, jv_str(keys[i].s, keys[i].len));
            } else {
                JVal *v = jv_obj_get_len(in, keys[i].s, keys[i].len);
                jv_arr_push(arr, v ? v : jv_null());
            }
        }
        free(keys);
        *out = arr;
        return 0;
    }

    if (strcmp(op, "merge") == 0) {
        static const char *const K[] = { "a", "b" };
        if (arg_keys(args, K, 2, e) < 0)
            return -1;
        JVal *a, *b;
        if (need_obj(args, "a", &a, e) < 0)
            return -1;
        if (need_obj(args, "b", &b, e) < 0)
            return -1;
        JVal *obj = jv_obj();
        for (size_t i = 0; i < a->u.o.n; i++)
            jv_obj_set(obj, a->u.o.k[i].s, a->u.o.k[i].len, a->u.o.v[i]);
        for (size_t i = 0; i < b->u.o.n; i++)
            jv_obj_set(obj, b->u.o.k[i].s, b->u.o.k[i].len, b->u.o.v[i]);
        *out = obj;
        return 0;
    }

    if (strcmp(op, "pick") == 0 || strcmp(op, "omit") == 0) {
        static const char *const K[] = { "input", "keys" };
        if (arg_keys(args, K, 2, e) < 0)
            return -1;
        JVal *in, *keysv;
        if (need_obj(args, "input", &in, e) < 0)
            return -1;
        if (need_arr(args, "keys", &keysv, e) < 0)
            return -1;
        JStr *keys = NULL;
        size_t nk = collect_sorted_keys(keysv, &keys, e);
        if (!keys)
            return -1;
        JVal *obj = jv_obj();
        if (strcmp(op, "pick") == 0) {
            for (size_t i = 0; i < nk; i++) {
                JVal *v = jv_obj_get_len(in, keys[i].s, keys[i].len);
                jv_obj_append(obj, keys[i].s, keys[i].len,
                              v ? v : jv_null());
            }
        } else {
            for (size_t i = 0; i < in->u.o.n; i++) {
                int omit = 0;
                for (size_t j = 0; j < nk; j++) {
                    if (cmp_jstr_local(&in->u.o.k[i], &keys[j]) == 0) {
                        omit = 1;
                        break;
                    }
                }
                if (!omit)
                    jv_obj_append(obj, in->u.o.k[i].s, in->u.o.k[i].len,
                                  in->u.o.v[i]);
            }
        }
        free(keys);
        *out = obj;
        return 0;
    }

    /* ---------------- utilities ---------------- */
    if (strcmp(op, "constant") == 0) {
        static const char *const K[] = { "value" };
        if (arg_keys(args, K, 1, e) < 0)
            return -1;
        JVal *v;
        if (need(args, "value", &v, e) < 0)
            return -1;
        *out = v;
        return 0;
    }

    if (strcmp(op, "equals") == 0) {
        static const char *const K[] = { "a", "b" };
        if (arg_keys(args, K, 2, e) < 0)
            return -1;
        JVal *a, *b;
        if (need(args, "a", &a, e) < 0)
            return -1;
        if (need(args, "b", &b, e) < 0)
            return -1;
        *out = jv_bool(jv_equal(a, b));
        return 0;
    }

    if (strcmp(op, "compare") == 0) {
        static const char *const K[] = { "a", "b", "op" };
        if (arg_keys(args, K, 3, e) < 0)
            return -1;
        JVal *a, *b;
        const JStr *cop;
        if (need(args, "a", &a, e) < 0)
            return -1;
        if (need(args, "b", &b, e) < 0)
            return -1;
        if (need_str(args, "op", &cop, e) < 0)
            return -1;
        int c = jv_compare(a, b);
        int r;
        if (jstr_eq_c(cop, "gt"))
            r = c > 0;
        else if (jstr_eq_c(cop, "gte"))
            r = c >= 0;
        else if (jstr_eq_c(cop, "lt"))
            r = c < 0;
        else if (jstr_eq_c(cop, "lte"))
            r = c <= 0;
        else {
            err_set(e, E_INVALID_ARG, NULL,
                    "compare 'op' must be gt, gte, lt or lte");
            return -1;
        }
        *out = jv_bool(r);
        return 0;
    }

    if (strcmp(op, "and") == 0 || strcmp(op, "or") == 0) {
        static const char *const K[] = { "a", "b" };
        if (arg_keys(args, K, 2, e) < 0)
            return -1;
        JVal *a, *b;
        if (need(args, "a", &a, e) < 0)
            return -1;
        if (need(args, "b", &b, e) < 0)
            return -1;
        int x = jv_truthy(a), y = jv_truthy(b);
        *out = jv_bool(strcmp(op, "and") == 0 ? (x && y) : (x || y));
        return 0;
    }

    if (strcmp(op, "not") == 0) {
        static const char *const K[] = { "a" };
        if (arg_keys(args, K, 1, e) < 0)
            return -1;
        JVal *a;
        if (need(args, "a", &a, e) < 0)
            return -1;
        *out = jv_bool(!jv_truthy(a));
        return 0;
    }

    if (strcmp(op, "type") == 0) {
        static const char *const K[] = { "value" };
        if (arg_keys(args, K, 1, e) < 0)
            return -1;
        JVal *v;
        if (need(args, "value", &v, e) < 0)
            return -1;
        *out = mkcstr(jv_type_name(v));
        return 0;
    }

    if (strcmp(op, "fail_until") == 0) {
        static const char *const K[] = { "failures", "value" };
        if (arg_keys(args, K, 2, e) < 0)
            return -1;
        int64_t failures;
        if (need_i64(args, "failures", &failures, e) < 0)
            return -1;
        if (failures < 0 || failures > 10) {
            err_set(e, E_INVALID_ARG, NULL,
                    "'failures' must be between 0 and 10");
            return -1;
        }
        JVal *v;
        if (need(args, "value", &v, e) < 0)
            return -1;
        st->fail_attempts++;
        if (st->fail_attempts <= failures) {
            err_set(e, E_TRANSIENT_FAILURE, NULL,
                    "transient failure on attempt %d", st->fail_attempts);
            return -1;
        }
        *out = v;
        return 0;
    }

    err_set(e, E_INVALID_ARG, NULL, "unknown operation '%s'", op);
    return -1;
}

/* ------------------------------------------------------------------ */
/* response emission                                                   */
/* ------------------------------------------------------------------ */

static int emit_response(Ctx *ctx, const char *err_code, const char *err_step,
                         const char *err_msg)
{
    JVal *resp = jv_obj();
    jv_obj_set(resp, "status", 6,
               mkcstr(err_code ? "failed" : "completed"));
    jv_obj_set(resp, "results", 7,
               ctx && ctx->results_obj ? ctx->results_obj : jv_obj());
    jv_obj_set(resp, "execution_order", 15,
               ctx && ctx->exec_order ? ctx->exec_order : jv_arr());
    jv_obj_set(resp, "skipped", 7,
               ctx && ctx->skipped_list ? ctx->skipped_list : jv_arr());
    if (err_code) {
        JVal *errobj = jv_obj();
        jv_obj_set(errobj, "code", 4, mkcstr(err_code));
        if (err_step)
            jv_obj_set(errobj, "step", 4, mkcstr(err_step));
        else
            jv_obj_set(errobj, "step", 4, jv_null());
        jv_obj_set(errobj, "message", 7,
                   mkcstr(err_msg ? err_msg : "error"));
        jv_obj_set(resp, "error", 5, errobj);
    } else {
        jv_obj_set(resp, "error", 5, jv_null());
    }
    size_t len = 0;
    char *s = jv_dump(resp, &len);
    fwrite(s, 1, len, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    free(s);
    return exit_for_code(err_code);
}

/* ------------------------------------------------------------------ */
/* execution                                                           */
/* ------------------------------------------------------------------ */

static int execute(Ctx *ctx, Err *e)
{
    for (;;) {
        int pick = -1;
        for (int i = 0; i < ctx->n; i++) {
            if (ctx->done[i])
                continue;
            int ready = 1;
            for (int k = 0; k < ctx->steps[i].ndeps; k++) {
                if (!ctx->done[ctx->steps[i].deps[k]]) {
                    ready = 0;
                    break;
                }
            }
            if (ready) {
                pick = i;
                break;
            }
        }
        if (pick < 0)
            break;

        Step *st = &ctx->steps[pick];

        if (st->when) {
            int ok = 1;
            JVal *cv = eval_condition(ctx, st->when, pick, e, &ok);
            if (!ok) {
                if (!e->step)
                    e->step = st->id;
                return -1;
            }
            if (!cv || cv->type != J_BOOL) {
                err_set(e, E_INVALID_CONDITION, st->id,
                        "condition did not evaluate to a boolean");
                return -1;
            }
            if (!cv->u.b) {
                JVal *n = jv_null();
                ctx->results[pick] = n;
                ctx->skipped[pick] = 1;
                ctx->done[pick] = 1;
                jv_obj_set(ctx->results_obj, st->id, strlen(st->id), n);
                jv_arr_push(ctx->exec_order, mkcstr(st->id));
                jv_arr_push(ctx->skipped_list, mkcstr(st->id));
                continue;
            }
        }

        int failed = 0;
        for (int attempt = 1; attempt <= st->max_attempts; attempt++) {
            Err op_err;
            memset(&op_err, 0, sizeof(op_err));
            int ok = 1;
            JVal *resolved = resolve_value(ctx, st->args, pick, &op_err, &ok);
            if (!ok) {
                op_err.step = st->id;
                *e = op_err;
                return -1;
            }
            JVal *result = NULL;
            if (run_op(ctx, pick, st->op, resolved, &result, &op_err) == 0) {
                ctx->results[pick] = result;
                ctx->done[pick] = 1;
                jv_obj_set(ctx->results_obj, st->id, strlen(st->id), result);
                jv_arr_push(ctx->exec_order, mkcstr(st->id));
                failed = 0;
                break;
            }
            failed = 1;
            if (strcmp(op_err.code, E_TRANSIENT_FAILURE) == 0 &&
                attempt < st->max_attempts)
                continue;
            if (strcmp(op_err.code, E_TRANSIENT_FAILURE) == 0)
                op_err.code = E_STEP_FAILED;
            op_err.step = st->id;
            *e = op_err;
            return -1;
        }
        if (failed)
            return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* input handling                                                      */
/* ------------------------------------------------------------------ */

static char *read_stream(FILE *f, size_t *out_len)
{
    size_t cap = 8192, len = 0;
    char *buf = malloc(cap);
    if (!buf)
        jv_oom();
    for (;;) {
        if (len == cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb)
                jv_oom();
            buf = nb;
        }
        size_t got = fread(buf + len, 1, cap - len, f);
        len += got;
        if (got == 0) {
            if (ferror(f)) {
                free(buf);
                return NULL;
            }
            break;
        }
    }
    if (len == cap) {
        char *nb = realloc(buf, cap + 1);
        if (!nb)
            jv_oom();
        buf = nb;
    }
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

int main(int argc, char **argv)
{
    if (argc != 3 || strcmp(argv[1], "run") != 0) {
        Ctx empty;
        memset(&empty, 0, sizeof(empty));
        empty.results_obj = jv_obj();
        empty.exec_order = jv_arr();
        empty.skipped_list = jv_arr();
        return emit_response(&empty, E_INVALID_WORKFLOW, NULL,
                             "usage: workflow run <workflow.json>");
    }

    char *input = NULL;
    size_t input_len = 0;
    if (strcmp(argv[2], "-") == 0) {
        input = read_stream(stdin, &input_len);
    } else {
        FILE *f = fopen(argv[2], "rb");
        if (!f) {
            Ctx empty;
            memset(&empty, 0, sizeof(empty));
            empty.results_obj = jv_obj();
            empty.exec_order = jv_arr();
            empty.skipped_list = jv_arr();
            return emit_response(&empty, E_INVALID_WORKFLOW, NULL,
                                 "cannot read input file");
        }
        input = read_stream(f, &input_len);
        fclose(f);
    }
    if (!input) {
        Ctx empty;
        memset(&empty, 0, sizeof(empty));
        empty.results_obj = jv_obj();
        empty.exec_order = jv_arr();
        empty.skipped_list = jv_arr();
        return emit_response(&empty, E_INVALID_WORKFLOW, NULL,
                             "cannot read input");
    }

    JVal *doc = NULL;
    char perrbuf[192];
    perrbuf[0] = '\0';
    if (jv_parse(input, input_len, &doc, perrbuf, sizeof(perrbuf)) < 0) {
        Ctx empty;
        memset(&empty, 0, sizeof(empty));
        empty.results_obj = jv_obj();
        empty.exec_order = jv_arr();
        empty.skipped_list = jv_arr();
        return emit_response(&empty, E_INVALID_WORKFLOW, NULL,
                             perrbuf[0] ? perrbuf : "invalid JSON");
    }
    free(input);

    Ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.doc = doc;
    ctx.results_obj = jv_obj();
    ctx.exec_order = jv_arr();
    ctx.skipped_list = jv_arr();

    Err e;
    memset(&e, 0, sizeof(e));
    if (validate_workflow(&ctx, &e) < 0)
        return emit_response(&ctx, e.code, e.step, e.msg);

    if (execute(&ctx, &e) < 0)
        return emit_response(&ctx, e.code, e.step, e.msg);

    return emit_response(&ctx, NULL, NULL, NULL);
}
