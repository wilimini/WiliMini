#ifndef BILI_JSON_H
#define BILI_JSON_H
#include <stddef.h>
typedef struct { int start, end, next, children; } JsonToken;
typedef struct { const char *text; JsonToken *tokens; int count; } Json;
int json_parse(Json *j, const char *text, JsonToken *tokens, int capacity);
int json_key(const Json *j, int object, const char *key);
int json_string(const Json *j, int token, char *out, size_t size);
long long json_number(const Json *j, int token);
#endif
