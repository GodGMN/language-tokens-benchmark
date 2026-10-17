/* Minimal, strict JSON value model, parser and compact serializer.
 *
 * Written for the workflow engine: integers that fit in i64 are preserved
 * exactly, numbers with a fraction/exponent are kept as doubles so they can
 * be distinguished when validating numeric parameters.
 */
#ifndef WORKFLOW_JSON_H
#define WORKFLOW_JSON_H

#include <stddef.h>
#include <stdint.h>

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct {
    char *s;      /* NUL terminated for convenience, may contain NUL bytes */
    size_t len;
} JStr;

typedef struct JVal JVal;

struct JVal {
    JType type;
    union {
        int b;
        struct {
            int is_int;   /* 1 -> use i; 0 -> use d */
            int64_t i;
            double d;
        } n;
        JStr s;
        struct {
            JVal **v;
            size_t n, cap;
        } a;
        struct {
            JStr *k;
            JVal **v;
            size_t n, cap;
        } o;
    } u;
};

JVal *jv_null(void);
JVal *jv_bool(int b);
JVal *jv_int(int64_t i);
JVal *jv_double(double d);
JVal *jv_str(const char *s, size_t len);
JVal *jv_cstr(const char *s);
JVal *jv_arr(void);
JVal *jv_obj(void);

void jv_arr_push(JVal *arr, JVal *v);
/* Append without duplicate-key handling (used by the parser). */
void jv_obj_append(JVal *obj, const char *key, size_t klen, JVal *v);
/* Replace the value of an existing key, otherwise append. */
void jv_obj_set(JVal *obj, const char *key, size_t klen, JVal *v);
JVal *jv_obj_get(const JVal *obj, const char *key); /* first match, NULL if absent */
JVal *jv_obj_get_len(const JVal *obj, const char *key, size_t klen);
int jv_obj_has(const JVal *obj, const char *key);

int jv_is_ref(const JVal *v);

/* Parse; returns 0 on success. On failure fills err and returns -1. */
int jv_parse(const char *buf, size_t len, JVal **out, char *err, size_t errsz);

/* Compact serialization; caller frees the returned buffer. */
char *jv_dump(const JVal *v, size_t *outlen);

int jv_equal(const JVal *a, const JVal *b);
/* Total order: returns <0, 0 or >0. */
int jv_compare(const JVal *a, const JVal *b);

const char *jv_type_name(const JVal *v);
int jv_truthy(const JVal *v);

void jv_oom(void); /* prints a minimal error document and exits 3 */

#endif
