#include "tool_registry.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

struct tool_handler {
    const char *name;
    int (*handler)(const cJSON *arguments, char **output_out);
};

struct string_buf {
    char *data;
    size_t size;
};

static const char *get_json_string(const cJSON *obj, const char *key) {
    if (!cJSON_IsObject((cJSON *)obj)) return NULL;

    cJSON *item = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL) return NULL;
    return item->valuestring;
}

static char *duplicate_string(const char *text) {
    if (!text) text = "";

    size_t len = strlen(text);
    char *copy = malloc(len + 1);
    if (!copy) return NULL;

    memcpy(copy, text, len + 1);
    return copy;
}

static int set_output_string(char **output_out, const char *text) {
    if (!output_out) return 0;

    char *copy = duplicate_string(text);
    if (!copy) {
        fprintf(stderr, "failed to allocate tool output\n");
        return 1;
    }

    *output_out = copy;
    return 0;
}

static int string_buf_append(struct string_buf *buf, const char *data, size_t size) {
    char *tmp = realloc(buf->data, buf->size + size + 1);
    if (!tmp) return 1;

    buf->data = tmp;
    memcpy(buf->data + buf->size, data, size);
    buf->size += size;
    buf->data[buf->size] = '\0';
    return 0;
}

static int ensure_parent_directories(const char *path) {
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

static int write_entire_file(const char *path, const char *content) {
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

static char *run_bash_command(const char *command) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        fprintf(stderr, "failed to create pipe for bash command: %s\n", strerror(errno));
        return NULL;
    }

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "failed to fork for bash command: %s\n", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        return NULL;
    }

    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execl("/bin/bash", "bash", "-lc", command, (char *)NULL);
        perror("failed to execute bash");
        _exit(127);
    }

    close(pipefd[1]);

    struct string_buf buf = {NULL, 0};
    char chunk[4096];
    for (;;) {
        ssize_t read_len = read(pipefd[0], chunk, sizeof(chunk));
        if (read_len < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "failed while reading bash output: %s\n", strerror(errno));
            free(buf.data);
            close(pipefd[0]);
            waitpid(pid, NULL, 0);
            return NULL;
        }
        if (read_len == 0) break;

        if (string_buf_append(&buf, chunk, (size_t)read_len) != 0) {
            fprintf(stderr, "failed to capture bash output\n");
            free(buf.data);
            close(pipefd[0]);
            waitpid(pid, NULL, 0);
            return NULL;
        }
    }

    close(pipefd[0]);
    if (waitpid(pid, NULL, 0) < 0) {
        fprintf(stderr, "failed to wait for bash command: %s\n", strerror(errno));
        free(buf.data);
        return NULL;
    }

    if (!buf.data) {
        return duplicate_string("");
    }

    return buf.data;
}

static int read_tool_handler(const cJSON *arguments, char **output_out) {
    const char *file_path = get_json_string(arguments, "file_path");
    if (!file_path) file_path = get_json_string(arguments, "path");
    if (!file_path) {
        fprintf(stderr, "Read tool call is missing file_path\n");
        return 1;
    }

    char *contents = read_entire_file(file_path, NULL);
    if (!contents) return 1;

    if (output_out) {
        *output_out = contents;
        return 0;
    }

    free(contents);
    return 0;
}

static int write_tool_handler(const cJSON *arguments, char **output_out) {
    const char *file_path = get_json_string(arguments, "file_path");
    if (!file_path) file_path = get_json_string(arguments, "path");
    const char *content = get_json_string(arguments, "content");
    if (!content) content = get_json_string(arguments, "contents");

    if (!file_path) {
        fprintf(stderr, "Write tool call is missing file_path\n");
        return 1;
    }
    if (!content) {
        fprintf(stderr, "Write tool call is missing content\n");
        return 1;
    }

    if (write_entire_file(file_path, content) != 0) {
        return 1;
    }

    char confirmation[1024];
    snprintf(confirmation, sizeof(confirmation), "Wrote %s", file_path);
    return set_output_string(output_out, confirmation);
}

static int bash_tool_handler(const cJSON *arguments, char **output_out) {
    const char *command = get_json_string(arguments, "command");
    if (!command) command = get_json_string(arguments, "bash_command");
    if (!command) command = get_json_string(arguments, "script");

    if (!command) {
        fprintf(stderr, "Bash tool call is missing command\n");
        return 1;
    }

    char *output = run_bash_command(command);
    if (!output) return 1;

    if (output_out) {
        *output_out = output;
        return 0;
    }

    free(output);
    return 0;
}

static cJSON *build_tool_schema(const char *name, const char *description, cJSON *parameters) {
    cJSON *tool = cJSON_CreateObject();
    cJSON *function = cJSON_CreateObject();

    if (!tool || !function || !parameters) {
        cJSON_Delete(tool);
        cJSON_Delete(function);
        cJSON_Delete(parameters);
        return NULL;
    }

    cJSON_AddStringToObject(tool, "type", "function");
    cJSON_AddStringToObject(function, "name", name);
    cJSON_AddStringToObject(function, "description", description);
    cJSON_AddItemToObject(function, "parameters", parameters);
    cJSON_AddItemToObject(tool, "function", function);
    return tool;
}

static cJSON *build_read_tool_schema(void) {
    cJSON *parameters = cJSON_CreateObject();
    cJSON *properties = cJSON_CreateObject();
    cJSON *required = cJSON_CreateArray();
    cJSON *file_path = cJSON_CreateObject();

    if (!parameters || !properties || !required || !file_path) {
        cJSON_Delete(parameters);
        cJSON_Delete(properties);
        cJSON_Delete(required);
        cJSON_Delete(file_path);
        return NULL;
    }

    cJSON_AddStringToObject(parameters, "type", "object");
    cJSON_AddItemToObject(parameters, "properties", properties);
    cJSON_AddItemToObject(parameters, "required", required);
    cJSON_AddBoolToObject(parameters, "additionalProperties", 0);

    cJSON_AddStringToObject(file_path, "type", "string");
    cJSON_AddStringToObject(file_path, "description", "Path to the file to read");
    cJSON_AddItemToObject(properties, "file_path", file_path);
    cJSON_AddItemToArray(required, cJSON_CreateString("file_path"));

    return build_tool_schema("Read", "Read and return the contents of a file", parameters);
}

static cJSON *build_write_tool_schema(void) {
    cJSON *parameters = cJSON_CreateObject();
    cJSON *properties = cJSON_CreateObject();
    cJSON *required = cJSON_CreateArray();
    cJSON *file_path = cJSON_CreateObject();
    cJSON *content = cJSON_CreateObject();

    if (!parameters || !properties || !required || !file_path || !content) {
        cJSON_Delete(parameters);
        cJSON_Delete(properties);
        cJSON_Delete(required);
        cJSON_Delete(file_path);
        cJSON_Delete(content);
        return NULL;
    }

    cJSON_AddStringToObject(parameters, "type", "object");
    cJSON_AddItemToObject(parameters, "properties", properties);
    cJSON_AddItemToObject(parameters, "required", required);
    cJSON_AddBoolToObject(parameters, "additionalProperties", 0);

    cJSON_AddStringToObject(file_path, "type", "string");
    cJSON_AddStringToObject(file_path, "description", "Path to the file to write");
    cJSON_AddItemToObject(properties, "file_path", file_path);

    cJSON_AddStringToObject(content, "type", "string");
    cJSON_AddStringToObject(content, "description", "Content to write to the file");
    cJSON_AddItemToObject(properties, "content", content);

    cJSON_AddItemToArray(required, cJSON_CreateString("file_path"));
    cJSON_AddItemToArray(required, cJSON_CreateString("content"));

    return build_tool_schema("Write", "Write content to a file", parameters);
}

static cJSON *build_bash_tool_schema(void) {
    cJSON *parameters = cJSON_CreateObject();
    cJSON *properties = cJSON_CreateObject();
    cJSON *required = cJSON_CreateArray();
    cJSON *command = cJSON_CreateObject();

    if (!parameters || !properties || !required || !command) {
        cJSON_Delete(parameters);
        cJSON_Delete(properties);
        cJSON_Delete(required);
        cJSON_Delete(command);
        return NULL;
    }

    cJSON_AddStringToObject(parameters, "type", "object");
    cJSON_AddItemToObject(parameters, "properties", properties);
    cJSON_AddItemToObject(parameters, "required", required);
    cJSON_AddBoolToObject(parameters, "additionalProperties", 0);

    cJSON_AddStringToObject(command, "type", "string");
    cJSON_AddStringToObject(command, "description", "Shell command to execute");
    cJSON_AddItemToObject(properties, "command", command);
    cJSON_AddItemToArray(required, cJSON_CreateString("command"));

    return build_tool_schema("Bash", "Execute a shell command and return its output", parameters);
}

cJSON *tool_registry_build_schema(void) {
    cJSON *tools = cJSON_CreateArray();
    cJSON *read_tool = build_read_tool_schema();
    cJSON *write_tool = build_write_tool_schema();
    cJSON *bash_tool = build_bash_tool_schema();

    if (!tools || !read_tool || !write_tool || !bash_tool) {
        cJSON_Delete(tools);
        cJSON_Delete(read_tool);
        cJSON_Delete(write_tool);
        cJSON_Delete(bash_tool);
        return NULL;
    }

    cJSON_AddItemToArray(tools, read_tool);
    cJSON_AddItemToArray(tools, write_tool);
    cJSON_AddItemToArray(tools, bash_tool);
    return tools;
}

int tool_registry_execute(const cJSON *tool_call, char **output_out) {
    static const struct tool_handler handlers[] = {
        {"Read", read_tool_handler},
        {"Write", write_tool_handler},
        {"Bash", bash_tool_handler},
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
