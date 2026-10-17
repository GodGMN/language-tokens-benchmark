/*
 * Minimal, self-contained JSON parser / serializer for the patch CLI.
 *
 * The representation is deliberately simple:
 *   - strings are stored as length-delimited UTF-8 bytes (may contain NUL),
 *   - numbers keep their original lexical form; comparison is numeric,
 *   - objects are insertion ordered and duplicate keys keep their last value.
 */
#ifndef PATCH_JSON_H
#define PATCH_JSON_H

#include <stddef.h>
#include <stdio.h>

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct Json Json;

typedef struct {
    char *key;
    size_t keylen;
    Json *value;
} JsonPair;

struct Json {
    JsonType type;
    union {
        int boolean;
        struct {
            char *lexeme;
            size_t len;
        } number;
        struct {
            char *bytes;
            size_t len;
        } string;
        struct {
            Json **items;
            size_t len;
            size_t cap;
        } array;
        struct {
            JsonPair *pairs;
            size_t len;
            size_t cap;
        } object;
    } u;
};

/* Parse a complete JSON document.  Returns NULL on syntax error. */
Json *json_parse(const char *buf, size_t len, char *errbuf, size_t errbuf_size);

void json_free(Json *value);
Json *json_clone(const Json *value);
int json_equal(const Json *a, const Json *b);
void json_print(FILE *out, const Json *value);

/* Length-aware object lookup.  Returns NULL when the key is absent. */
Json *json_object_get(const Json *object, const char *key, size_t keylen);

/*
 * Object/array mutation helpers.  These take ownership of inserted values and
 * free replaced values.  Allocation failure is an internal error (exit 3).
 */
void json_object_set(Json *object, const char *key, size_t keylen, Json *value);
Json *json_object_remove(Json *object, const char *key, size_t keylen);
void json_array_insert(Json *array, size_t index, Json *value);
Json *json_array_remove(Json *array, size_t index);

#endif /* PATCH_JSON_H */
