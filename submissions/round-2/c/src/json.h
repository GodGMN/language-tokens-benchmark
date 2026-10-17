#ifndef WF_JSON_H
#define WF_JSON_H

#include <stddef.h>
#include <stdint.h>

/* ---------- arena allocator ---------- */

typedef struct ArenaBlock ArenaBlock;
typedef struct {
    ArenaBlock *head;
} Arena;

void arena_init(Arena *a);
void *arena_alloc(Arena *a, size_t n);
char *arena_strndup(Arena *a, const char *s, size_t n);

/* ---------- growable byte buffer ---------- */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buf;

void buf_init(Buf *b);
void buf_free(Buf *b);
void buf_append(Buf *b, const char *s, size_t n);
void buf_putc(Buf *b, char c);

/* ---------- JSON values ---------- */

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JVal JVal;

typedef struct {
    char *key;
    size_t klen;
    JVal *val;
} JPair;

struct JVal {
    JType type;
    union {
        int boolean;
        struct {
            char *raw;   /* original literal (NUL-terminated), or NULL if synthesized */
            int is_int;  /* no fraction / exponent in the literal */
            int fits;    /* integer value fits in int64_t */
            int64_t i;   /* integer value when fits */
            double d;    /* numeric value as double (for comparisons) */
        } num;
        struct {
            char *s;
            size_t len;
        } str;
        struct {
            JVal **items;
            size_t len;
            size_t cap;
        } arr;
        struct {
            JPair *pairs;
            size_t len;
            size_t cap;
        } obj;
    } u;
};

JVal *jv_null(Arena *a);
JVal *jv_bool(Arena *a, int b);
JVal *jv_int(Arena *a, int64_t i);
JVal *jv_str(Arena *a, const char *s, size_t n);
JVal *jv_arr(Arena *a);
void jv_arr_push(Arena *a, JVal *arr, JVal *item);
JVal *jv_obj(Arena *a);
void jv_obj_set(Arena *a, JVal *obj, const char *key, size_t klen, JVal *val);
void jv_obj_append(Arena *a, JVal *obj, const char *key, size_t klen, JVal *val);
JVal *jv_obj_get_last(const JVal *obj, const char *key);
JVal *jv_obj_get_n(const JVal *obj, const char *key, size_t klen);
JVal *jv_obj_get_first_n(const JVal *obj, const char *key, size_t klen);

int cmp_bytes(const char *a, size_t alen, const char *b, size_t blen);
int jv_equal(const JVal *a, const JVal *b);
int jv_cmp(const JVal *a, const JVal *b);
int jv_truthy(const JVal *v);
const char *jv_type_name(const JVal *v);

/* ---------- parse / emit ---------- */

JVal *json_parse(Arena *a, const char *src, size_t len, const char **errmsg);
void json_write(Buf *b, const JVal *v);
void json_write_string(Buf *b, const char *s, size_t n);

#endif
