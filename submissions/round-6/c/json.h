#ifndef JSON_H
#define JSON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef enum { J_NULL = 0, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JVal JVal;

struct JVal {
    JType t;

    /* boolean */
    bool b;

    /* number */
    int64_t i;        /* used when is_int */
    bool is_int;      /* true when the literal was an integer that fits i64 */
    long double f;    /* numeric value as long double */
    const char *raw;  /* original number lexeme (may be NULL for generated) */

    /* string */
    char *s;
    size_t slen;

    /* array */
    JVal **items;
    size_t alen, acap;

    /* object */
    char **keys;
    size_t *klens;
    JVal **vals;
    size_t olen, ocap;
};

/* allocation helpers (abort on OOM) */
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrndup(const char *s, size_t n);

/* constructors */
JVal *jnull(void);
JVal *jbool(bool b);
JVal *jint(int64_t v);
JVal *jstr(const char *s);
JVal *jstrn(const char *s, size_t n);
JVal *jarr(void);
JVal *jobj(void);

void jarr_push(JVal *a, JVal *v);
void jobj_put(JVal *o, const char *k, size_t klen, JVal *v);
JVal *jobj_getn(const JVal *o, const char *k, size_t klen);
JVal *jobj_get(const JVal *o, const char *k);

JVal *jcopy(const JVal *v);
bool jeq(const JVal *a, const JVal *b);
int jcmp(const JVal *a, const JVal *b);
int jkeycmp(const char *a, size_t al, const char *b, size_t bl);
const char *jtype_name(const JVal *v);
bool jtruthy(const JVal *v);

JVal *jparse(const char *buf, size_t len, char *err, size_t errlen);

void jwrite(FILE *out, const JVal *v);
char *jwrite_alloc(const JVal *v, size_t *len);

#endif
