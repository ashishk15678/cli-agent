#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>

struct string_buf {
    char *data;
    size_t size;
};

char *duplicate_string(const char *text);
int set_output_string(char **output_out, const char *text);
int string_buf_append(struct string_buf *buf, const char *data, size_t size);
char *read_entire_file(const char *path, size_t *out_size);
int write_entire_file(const char *path, const char *content);
int ensure_parent_directories(const char *path);

#endif
