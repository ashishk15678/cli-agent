#include "tool_registry.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct tool_handler {
    const char *name;
    int (*handler)(const cJSON *arguments, char **output_out);
};

static const char *get_json_string(const cJSON *obj, const char *key) {
    if (!cJSON_IsObject((cJSON *)obj)) return NULL;

    cJSON *item = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL) return NULL;
    return item->valuestring;
}

static char *read_entire_file(const char *path, size_t *out_size) {
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

static int read_tool_handler(const cJSON *arguments, char **output_out) {
    const char *file_path = get_json_string(arguments, "file_path");
    if (!file_path) {
        fprintf(stderr, "Read tool call is missing file_path\n");
        return 1;
    }

    size_t size = 0;
    char *contents = read_entire_file(file_path, &size);
    if (!contents) return 1;

    if (output_out) {
        *output_out = contents;
        return 0;
    }

    free(contents);
    return 1;
}

static cJSON *build_read_tool_schema(void) {
    cJSON *tool = cJSON_CreateObject();
    cJSON *function = cJSON_CreateObject();
    cJSON *parameters = cJSON_CreateObject();
    cJSON *properties = cJSON_CreateObject();
    cJSON *required = cJSON_CreateArray();
    cJSON *file_path = cJSON_CreateObject();

    if (!tool || !function || !parameters || !properties || !required || !file_path) {
        cJSON_Delete(tool);
        cJSON_Delete(function);
        cJSON_Delete(parameters);
        cJSON_Delete(properties);
        cJSON_Delete(required);
        cJSON_Delete(file_path);
        return NULL;
    }

    cJSON_AddStringToObject(tool, "type", "function");
    cJSON_AddStringToObject(function, "name", "Read");
    cJSON_AddStringToObject(function, "description", "Read and return the contents of a file");

    cJSON_AddStringToObject(parameters, "type", "object");
    cJSON_AddItemToObject(parameters, "properties", properties);
    cJSON_AddItemToObject(parameters, "required", required);
    cJSON_AddBoolToObject(parameters, "additionalProperties", 0);

    cJSON_AddStringToObject(file_path, "type", "string");
    cJSON_AddStringToObject(file_path, "description", "Path to the file to read");
    cJSON_AddItemToObject(properties, "file_path", file_path);
    cJSON_AddItemToArray(required, cJSON_CreateString("file_path"));

    cJSON_AddItemToObject(function, "parameters", parameters);
    cJSON_AddItemToObject(tool, "function", function);
    return tool;
}

cJSON *tool_registry_build_schema(void) {
    cJSON *tools = cJSON_CreateArray();
    cJSON *read_tool = build_read_tool_schema();

    if (!tools || !read_tool) {
        cJSON_Delete(tools);
        cJSON_Delete(read_tool);
        return NULL;
    }

    cJSON_AddItemToArray(tools, read_tool);
    return tools;
}

int tool_registry_execute(const cJSON *tool_call, char **output_out) {
    static const struct tool_handler handlers[] = {
        {"Read", read_tool_handler},
    };

    if (output_out) {
        *output_out = NULL;
    }

    if (!cJSON_IsObject((cJSON *)tool_call)) {
        fprintf(stderr, "invalid tool call\n");
        return 1;
    }

    cJSON *function = cJSON_GetObjectItemCaseSensitive((cJSON *)tool_call, "function");
    const char *name = get_json_string(function, "name");
    const char *arguments_str = get_json_string(function, "arguments");

    if (!cJSON_IsObject(function) || !name) {
        fprintf(stderr, "tool call is missing function name\n");
        return 1;
    }

    if (!arguments_str) {
        fprintf(stderr, "tool call %s is missing arguments\n", name);
        return 1;
    }

    cJSON *arguments = cJSON_Parse(arguments_str);
    if (!arguments) {
        fprintf(stderr, "failed to parse arguments for tool %s\n", name);
        return 1;
    }

    size_t handler_count = sizeof(handlers) / sizeof(handlers[0]);
    for (size_t i = 0; i < handler_count; ++i) {
        if (strcmp(name, handlers[i].name) == 0) {
            int result = handlers[i].handler(arguments, output_out);
            cJSON_Delete(arguments);
            return result;
        }
    }

    fprintf(stderr, "unsupported tool: %s\n", name);
    cJSON_Delete(arguments);
    return 1;
}
