#include "json.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Error reporting                                                           */
/* ------------------------------------------------------------------------- */

typedef struct {
    const char *code;
    long index; /* -1 means JSON null */
    const char *message;
} PatchError;

static void set_error(PatchError *err, const char *code, long index,
                      const char *message)
{
    err->code = code;
    err->index = index;
    err->message = message;
}

static int exit_code_for(const char *code)
{
    if (strcmp(code, "INVALID_REQUEST") == 0)
        return 2;
    return 1;
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n == 0 ? 1 : n);
    if (p == NULL) {
        fprintf(stderr, "patch: out of memory\n");
        exit(3);
    }
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n == 0 ? 1 : n);
    if (q == NULL) {
        fprintf(stderr, "patch: out of memory\n");
        exit(3);
    }
    return q;
}

static int memeq_lit(const char *s, size_t len, const char *lit)
{
    size_t n = strlen(lit);
    return len == n && (n == 0 || memcmp(s, lit, n) == 0);
}

static int json_string_is(const Json *v, const char *lit)
{
    return v != NULL && v->type == JSON_STRING &&
           memeq_lit(v->u.string.bytes, v->u.string.len, lit);
}

/* ------------------------------------------------------------------------- */
/* JSON Pointer (RFC 6901)                                                   */
/* ------------------------------------------------------------------------- */

typedef struct {
    char **seg;
    size_t *len;
    size_t n;
} Pointer;

static void pointer_free(Pointer *p)
{
    size_t i;
    for (i = 0; i < p->n; i++)
        free(p->seg[i]);
    free(p->seg);
    free(p->len);
    p->seg = NULL;
    p->len = NULL;
    p->n = 0;
}

static int pointer_parse(const Json *path, Pointer *out, PatchError *err,
                         long index)
{
    const char *s;
    size_t len;
    size_t i;
    size_t count = 0;
    size_t pos;

    memset(out, 0, sizeof(*out));
    if (path == NULL || path->type != JSON_STRING) {
        set_error(err, "INVALID_REQUEST", index, "path must be a string");
        return -1;
    }
    s = path->u.string.bytes;
    len = path->u.string.len;

    if (len == 0)
        return 0;
    if (s[0] != '/') {
        set_error(err, "INVALID_PATH", index,
                  "path must be empty or start with /");
        return -1;
    }

    for (i = 0; i < len; i++) {
        if (s[i] == '/')
            count++;
    }
    out->seg = xmalloc(count * sizeof(char *));
    out->len = xmalloc(count * sizeof(size_t));

    pos = 1;
    for (i = 0; i < count; i++) {
        size_t start = pos;
        size_t end = pos;
        size_t j;
        size_t o = 0;
        char *tmp;

        while (end < len && s[end] != '/')
            end++;

        tmp = xmalloc((end - start) + 1);
        for (j = start; j < end; j++) {
            unsigned char c = (unsigned char)s[j];
            if (c == '~') {
                if (j + 1 >= end) {
                    free(tmp);
                    pointer_free(out);
                    set_error(err, "INVALID_PATH", index,
                              "dangling ~ escape in pointer");
                    return -1;
                }
                if (s[j + 1] == '0') {
                    tmp[o++] = '~';
                    j++;
                } else if (s[j + 1] == '1') {
                    tmp[o++] = '/';
                    j++;
                } else {
                    free(tmp);
                    pointer_free(out);
                    set_error(err, "INVALID_PATH", index,
                              "invalid ~ escape in pointer");
                    return -1;
                }
            } else {
                tmp[o++] = (char)c;
            }
        }
        tmp[o] = '\0';
        out->seg[i] = tmp;
        out->len[i] = o;
        out->n++;

        pos = end;
        if (pos < len && s[pos] == '/')
            pos++;
    }

    return 0;
}

static int pointer_equal(const Pointer *a, const Pointer *b)
{
    size_t i;
    if (a->n != b->n)
        return 0;
    for (i = 0; i < a->n; i++) {
        if (a->len[i] != b->len[i])
            return 0;
        if (a->len[i] != 0 &&
            memcmp(a->seg[i], b->seg[i], a->len[i]) != 0)
            return 0;
    }
    return 1;
}

/* Is desc a strict descendant of anc? */
static int pointer_strict_descendant(const Pointer *anc, const Pointer *desc)
{
    size_t i;
    if (anc->n >= desc->n)
        return 0;
    for (i = 0; i < anc->n; i++) {
        if (anc->len[i] != desc->len[i])
            return 0;
        if (anc->len[i] != 0 &&
            memcmp(anc->seg[i], desc->seg[i], anc->len[i]) != 0)
            return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Array index tokens                                                        */
/* ------------------------------------------------------------------------- */

static int token_is_dash(const Pointer *p, size_t index)
{
    return p->len[index] == 1 && p->seg[index][0] == '-';
}

/*
 * Valid array index is "0" or [1-9][0-9]*.
 * Returns 0 for a malformed token (including leading zeros and overflow).
 */
static int token_index(const char *s, size_t len, size_t *out)
{
    size_t i;
    size_t v = 0;

    if (len == 0)
        return 0;
    if (s[0] == '0') {
        if (len != 1)
            return 0;
        *out = 0;
        return 1;
    }
    if (s[0] < '1' || s[0] > '9')
        return 0;

    for (i = 0; i < len; i++) {
        unsigned d;
        if (s[i] < '0' || s[i] > '9')
            return 0;
        d = (unsigned)(s[i] - '0');
        if (v > (SIZE_MAX - d) / 10)
            return 0;
        v = v * 10 + d;
    }
    *out = v;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Navigation                                                                */
/* ------------------------------------------------------------------------- */

static int resolve_parent(Json *root, const Pointer *p, Json **parent_out,
                          size_t *last_out, PatchError *err)
{
    Json *cur = root;
    size_t i;

    for (i = 0; i + 1 < p->n; i++) {
        if (cur->type == JSON_OBJECT) {
            Json *next = json_object_get(cur, p->seg[i], p->len[i]);
            if (next == NULL) {
                set_error(err, "PATH_NOT_FOUND", -1,
                          "object member does not exist");
                return -1;
            }
            cur = next;
        } else if (cur->type == JSON_ARRAY) {
            size_t idx;
            if (token_is_dash(p, i) ||
                !token_index(p->seg[i], p->len[i], &idx)) {
                set_error(err, "INDEX_ERROR", -1, "invalid array index");
                return -1;
            }
            if (idx >= cur->u.array.len) {
                set_error(err, "INDEX_ERROR", -1,
                          "array index out of range");
                return -1;
            }
            cur = cur->u.array.items[idx];
        } else {
            set_error(err, "PATH_NOT_FOUND", -1,
                      "cannot descend into a scalar");
            return -1;
        }
    }

    *parent_out = cur;
    *last_out = p->n - 1;
    return 0;
}

static int get_value(Json *root, const Pointer *p, Json **out, PatchError *err)
{
    Json *cur = root;
    size_t i;

    for (i = 0; i < p->n; i++) {
        if (cur->type == JSON_OBJECT) {
            Json *next = json_object_get(cur, p->seg[i], p->len[i]);
            if (next == NULL) {
                set_error(err, "PATH_NOT_FOUND", -1,
                          "object member does not exist");
                return -1;
            }
            cur = next;
        } else if (cur->type == JSON_ARRAY) {
            size_t idx;
            if (token_is_dash(p, i) ||
                !token_index(p->seg[i], p->len[i], &idx)) {
                set_error(err, "INDEX_ERROR", -1, "invalid array index");
                return -1;
            }
            if (idx >= cur->u.array.len) {
                set_error(err, "INDEX_ERROR", -1,
                          "array index out of range");
                return -1;
            }
            cur = cur->u.array.items[idx];
        } else {
            set_error(err, "PATH_NOT_FOUND", -1,
                      "cannot descend into a scalar");
            return -1;
        }
    }

    *out = cur;
    return 0;
}

/* Remove a value from the document and return it (still allocated). */
static Json *detach_at(Json *root, const Pointer *p, PatchError *err)
{
    Json *parent;
    size_t last;

    if (p->n == 0) {
        set_error(err, "INVALID_PATH", -1, "cannot remove the root");
        return NULL;
    }
    if (resolve_parent(root, p, &parent, &last, err) != 0)
        return NULL;

    if (parent->type == JSON_OBJECT) {
        Json *v = json_object_remove(parent, p->seg[last], p->len[last]);
        if (v == NULL)
            set_error(err, "PATH_NOT_FOUND", -1,
                      "object member does not exist");
        return v;
    }
    if (parent->type == JSON_ARRAY) {
        size_t idx;
        if (token_is_dash(p, last) ||
            !token_index(p->seg[last], p->len[last], &idx)) {
            set_error(err, "INDEX_ERROR", -1, "invalid array index");
            return NULL;
        }
        if (idx >= parent->u.array.len) {
            set_error(err, "INDEX_ERROR", -1,
                      "array index out of range");
            return NULL;
        }
        return json_array_remove(parent, idx);
    }

    set_error(err, "PATH_NOT_FOUND", -1, "parent is not a container");
    return NULL;
}

/* Insert/replace according to add semantics.  Takes ownership of value. */
static int add_at(Json **root, const Pointer *p, Json *value, PatchError *err)
{
    Json *parent;
    size_t last;

    if (p->n == 0) {
        json_free(*root);
        *root = value;
        return 0;
    }

    if (resolve_parent(*root, p, &parent, &last, err) != 0)
        return -1;

    if (parent->type == JSON_OBJECT) {
        json_object_set(parent, p->seg[last], p->len[last], value);
        return 0;
    }
    if (parent->type == JSON_ARRAY) {
        size_t idx;
        if (token_is_dash(p, last)) {
            idx = parent->u.array.len;
        } else if (!token_index(p->seg[last], p->len[last], &idx)) {
            set_error(err, "INDEX_ERROR", -1, "invalid array index");
            return -1;
        }
        if (idx > parent->u.array.len) {
            set_error(err, "INDEX_ERROR", -1,
                      "array index out of range");
            return -1;
        }
        json_array_insert(parent, idx, value);
        return 0;
    }

    set_error(err, "PATH_NOT_FOUND", -1, "parent is not a container");
    return -1;
}

/* Replace an existing member.  Takes ownership of value. */
static int replace_at(Json **root, const Pointer *p, Json *value,
                      PatchError *err)
{
    Json *parent;
    size_t last;

    if (p->n == 0) {
        json_free(*root);
        *root = value;
        return 0;
    }

    if (resolve_parent(*root, p, &parent, &last, err) != 0)
        return -1;

    if (parent->type == JSON_OBJECT) {
        Json *old = json_object_get(parent, p->seg[last], p->len[last]);
        if (old == NULL) {
            set_error(err, "PATH_NOT_FOUND", -1,
                      "object member does not exist");
            return -1;
        }
        json_object_set(parent, p->seg[last], p->len[last], value);
        return 0;
    }
    if (parent->type == JSON_ARRAY) {
        size_t idx;
        if (token_is_dash(p, last) ||
            !token_index(p->seg[last], p->len[last], &idx)) {
            set_error(err, "INDEX_ERROR", -1, "invalid array index");
            return -1;
        }
        if (idx >= parent->u.array.len) {
            set_error(err, "INDEX_ERROR", -1,
                      "array index out of range");
            return -1;
        }
        json_free(parent->u.array.items[idx]);
        parent->u.array.items[idx] = value;
        return 0;
    }

    set_error(err, "PATH_NOT_FOUND", -1, "parent is not a container");
    return -1;
}

/* ------------------------------------------------------------------------- */
/* Operations                                                                */
/* ------------------------------------------------------------------------- */

typedef enum {
    OP_ADD,
    OP_REMOVE,
    OP_REPLACE,
    OP_MOVE,
    OP_COPY,
    OP_TEST
} OpKind;

static int field_allowed(const JsonPair *pair, OpKind kind)
{
    if (memeq_lit(pair->key, pair->keylen, "op") ||
        memeq_lit(pair->key, pair->keylen, "path"))
        return 1;
    if (kind == OP_ADD || kind == OP_REPLACE || kind == OP_TEST)
        return memeq_lit(pair->key, pair->keylen, "value");
    if (kind == OP_MOVE || kind == OP_COPY)
        return memeq_lit(pair->key, pair->keylen, "from");
    return 0;
}

static int parse_op(Json *op, long index, OpKind *kind_out, Json **path_out,
                    Json **from_out, Json **value_out, PatchError *err)
{
    Json *opname;
    Json *path;
    Json *extra;
    size_t i;

    *path_out = NULL;
    *from_out = NULL;
    *value_out = NULL;

    if (op->type != JSON_OBJECT) {
        set_error(err, "INVALID_REQUEST", index,
                  "patch operation must be an object");
        return -1;
    }

    opname = json_object_get(op, "op", 2);
    if (opname == NULL || opname->type != JSON_STRING) {
        set_error(err, "INVALID_REQUEST", index,
                  "operation must have a string op field");
        return -1;
    }

    if (json_string_is(opname, "add"))
        *kind_out = OP_ADD;
    else if (json_string_is(opname, "remove"))
        *kind_out = OP_REMOVE;
    else if (json_string_is(opname, "replace"))
        *kind_out = OP_REPLACE;
    else if (json_string_is(opname, "move"))
        *kind_out = OP_MOVE;
    else if (json_string_is(opname, "copy"))
        *kind_out = OP_COPY;
    else if (json_string_is(opname, "test"))
        *kind_out = OP_TEST;
    else {
        set_error(err, "UNKNOWN_OP", index, "unknown operation");
        return -1;
    }

    path = json_object_get(op, "path", 4);
    if (path == NULL || path->type != JSON_STRING) {
        set_error(err, "INVALID_REQUEST", index,
                  "operation must have a string path field");
        return -1;
    }

    for (i = 0; i < op->u.object.len; i++) {
        if (!field_allowed(&op->u.object.pairs[i], *kind_out)) {
            set_error(err, "INVALID_REQUEST", index,
                      "unexpected field in operation");
            return -1;
        }
    }

    if (*kind_out == OP_ADD || *kind_out == OP_REPLACE ||
        *kind_out == OP_TEST) {
        extra = json_object_get(op, "value", 5);
        if (extra == NULL) {
            set_error(err, "INVALID_REQUEST", index,
                      "operation requires a value field");
            return -1;
        }
        *value_out = extra;
    } else if (*kind_out == OP_MOVE || *kind_out == OP_COPY) {
        extra = json_object_get(op, "from", 4);
        if (extra == NULL || extra->type != JSON_STRING) {
            set_error(err, "INVALID_REQUEST", index,
                      "operation requires a string from field");
            return -1;
        }
        *from_out = extra;
    }

    *path_out = path;
    return 0;
}

static int apply_op(Json **root, Json *op, long index, PatchError *err)
{
    OpKind kind;
    Json *path_json;
    Json *from_json;
    Json *value_json;
    Pointer path;
    Pointer from;
    int have_path = 0;
    int have_from = 0;
    int rc = -1;

    if (parse_op(op, index, &kind, &path_json, &from_json, &value_json,
                 err) != 0)
        return -1;

    if (pointer_parse(path_json, &path, err, index) != 0)
        return -1;
    have_path = 1;

    switch (kind) {
    case OP_ADD: {
        Json *value = json_clone(value_json);
        if (add_at(root, &path, value, err) != 0) {
            json_free(value);
            goto done;
        }
        rc = 0;
        break;
    }

    case OP_REMOVE: {
        Json *removed;
        if (path.n == 0) {
            set_error(err, "INVALID_PATH", index,
                      "cannot remove the document root");
            goto done;
        }
        removed = detach_at(*root, &path, err);
        if (removed == NULL)
            goto done;
        json_free(removed);
        rc = 0;
        break;
    }

    case OP_REPLACE: {
        Json *value = json_clone(value_json);
        if (replace_at(root, &path, value, err) != 0) {
            json_free(value);
            goto done;
        }
        rc = 0;
        break;
    }

    case OP_MOVE: {
        Json *existing;
        Json *detached;

        if (pointer_parse(from_json, &from, err, index) != 0)
            goto done;
        have_from = 1;

        if (get_value(*root, &from, &existing, err) != 0)
            goto done;
        (void)existing;

        if (pointer_equal(&from, &path)) {
            set_error(err, "INVALID_MOVE", index,
                      "move source and destination are identical");
            goto done;
        }
        if (pointer_strict_descendant(&from, &path)) {
            set_error(err, "INVALID_MOVE", index,
                      "cannot move a value into its own descendant");
            goto done;
        }

        detached = detach_at(*root, &from, err);
        if (detached == NULL)
            goto done;
        if (add_at(root, &path, detached, err) != 0) {
            json_free(detached);
            goto done;
        }
        rc = 0;
        break;
    }

    case OP_COPY: {
        Json *existing;
        Json *copy;

        if (pointer_parse(from_json, &from, err, index) != 0)
            goto done;
        have_from = 1;

        if (get_value(*root, &from, &existing, err) != 0)
            goto done;
        copy = json_clone(existing);
        if (add_at(root, &path, copy, err) != 0) {
            json_free(copy);
            goto done;
        }
        rc = 0;
        break;
    }

    case OP_TEST: {
        Json *actual;
        if (get_value(*root, &path, &actual, err) != 0)
            goto done;
        if (!json_equal(actual, value_json)) {
            set_error(err, "TEST_FAILED", index,
                      "test value does not match");
            goto done;
        }
        rc = 0;
        break;
    }
    }

done:
    if (have_path)
        pointer_free(&path);
    if (have_from)
        pointer_free(&from);
    /* Fill in the operation index for errors raised during navigation. */
    if (rc != 0)
        err->index = index;
    return rc;
}

/* ------------------------------------------------------------------------- */
/* Request I/O                                                               */
/* ------------------------------------------------------------------------- */

static char *read_all(FILE *f, size_t *outlen)
{
    size_t cap = 4096;
    size_t len = 0;
    char *buf = xmalloc(cap);

    for (;;) {
        size_t n;
        if (len == cap) {
            cap *= 2;
            buf = xrealloc(buf, cap);
        }
        n = fread(buf + len, 1, cap - len, f);
        len += n;
        if (n == 0) {
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

static void print_error_json(const PatchError *err)
{
    fputs("{\"status\":\"error\",\"code\":\"", stdout);
    fputs(err->code, stdout);
    fputs("\",\"index\":", stdout);
    if (err->index < 0)
        fputs("null", stdout);
    else
        printf("%ld", err->index);
    fputs(",\"message\":\"", stdout);
    fputs(err->message != NULL ? err->message : "", stdout);
    fputs("\"}\n", stdout);
}

static int fail_request(const char *message)
{
    PatchError err;
    set_error(&err, "INVALID_REQUEST", -1, message);
    print_error_json(&err);
    return 2;
}

int main(int argc, char **argv)
{
    FILE *input;
    char *buf;
    size_t buflen = 0;
    char parse_error[128];
    Json *request;
    Json *document_json = NULL;
    Json *patch_json = NULL;
    Json *document;
    PatchError err;
    size_t i;
    int rc;

    if (argc != 3 || strcmp(argv[1], "apply") != 0)
        return fail_request("usage: patch apply <request.json|->");

    if (strcmp(argv[2], "-") == 0) {
        input = stdin;
    } else {
        input = fopen(argv[2], "rb");
        if (input == NULL) {
            fprintf(stderr, "patch: cannot open %s: %s\n", argv[2],
                    strerror(errno));
            return fail_request("cannot open request file");
        }
    }

    buf = read_all(input, &buflen);
    if (input != stdin)
        fclose(input);
    if (buf == NULL)
        return fail_request("cannot read request");

    request = json_parse(buf, buflen, parse_error, sizeof(parse_error));
    free(buf);
    if (request == NULL) {
        fprintf(stderr, "patch: %s\n", parse_error);
        return fail_request("request is not valid JSON");
    }

    if (request->type != JSON_OBJECT) {
        json_free(request);
        return fail_request("request must be an object");
    }

    for (i = 0; i < request->u.object.len; i++) {
        const JsonPair *pair = &request->u.object.pairs[i];
        if (memeq_lit(pair->key, pair->keylen, "document"))
            document_json = pair->value;
        else if (memeq_lit(pair->key, pair->keylen, "patch"))
            patch_json = pair->value;
        else {
            json_free(request);
            return fail_request("unknown top-level field");
        }
    }

    if (document_json == NULL || patch_json == NULL ||
        patch_json->type != JSON_ARRAY) {
        json_free(request);
        return fail_request("request needs document and array patch");
    }

    document = json_clone(document_json);

    for (i = 0; i < patch_json->u.array.len; i++) {
        if (apply_op(&document, patch_json->u.array.items[i], (long)i,
                     &err) != 0) {
            int code = exit_code_for(err.code);
            json_free(document);
            json_free(request);
            print_error_json(&err);
            return code;
        }
    }

    fputs("{\"status\":\"ok\",\"document\":", stdout);
    json_print(stdout, document);
    fputs("}\n", stdout);

    json_free(document);
    json_free(request);

    if (fflush(stdout) != 0) {
        fprintf(stderr, "patch: cannot write response\n");
        return 3;
    }
    rc = 0;
    return rc;
}
