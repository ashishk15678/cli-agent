#include "utils.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

char *duplicate_string(const char *text) {
    if (!text) text = "";
    size_t len = strlen(text);
    char *copy = malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, text, len + 1);
    return copy;
}

int set_output_string(char **output_out, const char *text) {
    if (!output_out) return 0;
    char *copy = duplicate_string(text);
    if (!copy) {
        fprintf(stderr, "failed to allocate output string\n");
        return 1;
    }
    *output_out = copy;
    return 0;
}

int string_buf_append(struct string_buf *buf, const char *data, size_t size) {
    char *tmp = realloc(buf->data, buf->size + size + 1);
    if (!tmp) return 1;
    buf->data = tmp;
    memcpy(buf->data + buf->size, data, size);
    buf->size += size;
    buf->data[buf->size] = '\0';
    return 0;
}

int ensure_parent_directories(const char *path) {
    if (!path || !*path) return 1;

    size_t len = strlen(path);
    char *mutable_path = malloc(len + 1);
    if (!mutable_path) {
        fprintf(stderr, "failed to allocate path buffer\n");
        return 1;
    }
    memcpy(mutable_path, path, len + 1);

    for (char *cursor = mutable_path + 1; *cursor; ++cursor) {
        if (*cursor != '/') continue;

        *cursor = '\0';
        if (mutable_path[0] != '\0') {
            if (mkdir(mutable_path, 0777) != 0 && errno != EEXIST) {
                fprintf(stderr, "failed to create directory %s: %s\n", mutable_path, strerror(errno));
                free(mutable_path);
                return 1;
            }
        }
        *cursor = '/';
    }

    free(mutable_path);
    return 0;
}

char *read_entire_file(const char *path, size_t *out_size) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "failed to open %s: %s\n", path, strerror(errno));
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fprintf(stderr, "failed to seek %s: %s\n", path, strerror(errno));
        fclose(file);
        return NULL;
    }

    long len = ftell(file);
    if (len < 0) {
        fprintf(stderr, "failed to determine size of %s: %s\n", path, strerror(errno));
        fclose(file);
        return NULL;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "failed to rewind %s: %s\n", path, strerror(errno));
        fclose(file);
        return NULL;
    }

    char *data = malloc((size_t)len + 1);
    if (!data) {
        fprintf(stderr, "failed to allocate %ld bytes\n", len + 1);
        fclose(file);
        return NULL;
    }

    size_t read_len = fread(data, 1, (size_t)len, file);
    if (read_len != (size_t)len) {
        if (ferror(file)) {
            fprintf(stderr, "failed to read %s: %s\n", path, strerror(errno));
        } else {
            fprintf(stderr, "unexpected end of file while reading %s\n", path);
        }
        free(data);
        fclose(file);
        return NULL;
    }

    data[read_len] = '\0';
    fclose(file);

    if (out_size) {
        *out_size = read_len;
    }

    return data;
}

int write_entire_file(const char *path, const char *content) {
    if (ensure_parent_directories(path) != 0) return 1;

    FILE *file = fopen(path, "wb");
    if (!file) {
        fprintf(stderr, "failed to open %s for writing: %s\n", path, strerror(errno));
        return 1;
    }

    size_t len = content ? strlen(content) : 0;
    if (len > 0 && fwrite(content, 1, len, file) != len) {
        fprintf(stderr, "failed to write %s: %s\n", path, strerror(errno));
        fclose(file);
        return 1;
    }

    if (fclose(file) != 0) {
        fprintf(stderr, "failed to close %s: %s\n", path, strerror(errno));
        return 1;
    }

    return 0;
}
