#include "tool_registry.h"
#include "utils.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

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
    if (!contents) {
        char err_msg[512];
        snprintf(err_msg, sizeof(err_msg), "Error: Failed to read file '%s': %s", file_path, strerror(errno));
        return set_output_string(output_out, err_msg);
    }

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
        char err_msg[512];
        snprintf(err_msg, sizeof(err_msg), "Error: Failed to write file '%s': %s", file_path, strerror(errno));
        return set_output_string(output_out, err_msg);
    }

    char confirmation[1024];
    snprintf(confirmation, sizeof(confirmation), "Successfully wrote %s", file_path);
    return set_output_string(output_out, confirmation);
}

static int create_file_tool_handler(const cJSON *arguments, char **output_out) {
    const char *file_path = get_json_string(arguments, "file_path");
    if (!file_path) file_path = get_json_string(arguments, "path");
    if (!file_path) {
        fprintf(stderr, "CreateFile tool call is missing file_path\n");
        return 1;
    }

    // Ensure parent directories exist
    ensure_parent_directories(file_path);

    // Open file for writing and close it to touch/create it
    FILE *file = fopen(file_path, "wb");
    if (!file) {
        char err_msg[512];
        snprintf(err_msg, sizeof(err_msg), "Error: Failed to create file '%s': %s", file_path, strerror(errno));
        return set_output_string(output_out, err_msg);
    }
    fclose(file);

    char confirmation[1024];
    snprintf(confirmation, sizeof(confirmation), "Successfully created empty file '%s'", file_path);
    return set_output_string(output_out, confirmation);
}

static int create_directory_tool_handler(const cJSON *arguments, char **output_out) {
    const char *dir_path = get_json_string(arguments, "directory_path");
    if (!dir_path) dir_path = get_json_string(arguments, "path");
    if (!dir_path) {
        fprintf(stderr, "CreateDirectory tool call is missing directory_path\n");
        return 1;
    }

    // Append trailing slash to ensure ensure_parent_directories creates the full directory path
    size_t len = strlen(dir_path);
    char *path_with_slash = malloc(len + 2);
    if (!path_with_slash) return 1;
    strcpy(path_with_slash, dir_path);
    if (len > 0 && path_with_slash[len - 1] != '/') {
        path_with_slash[len] = '/';
        path_with_slash[len + 1] = '\0';
    }

    if (ensure_parent_directories(path_with_slash) != 0) {
        free(path_with_slash);
        char err_msg[512];
        snprintf(err_msg, sizeof(err_msg), "Error: Failed to create directory '%s'", dir_path);
        return set_output_string(output_out, err_msg);
    }
    free(path_with_slash);

    char confirmation[1024];
    snprintf(confirmation, sizeof(confirmation), "Successfully created directory '%s'", dir_path);
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
    if (!output) {
        char err_msg[512];
        snprintf(err_msg, sizeof(err_msg), "Error: Failed to execute bash command: %s", strerror(errno));
        return set_output_string(output_out, err_msg);
    }

    if (output_out) {
        *output_out = output;
        return 0;
    }

    free(output);
    return 0;
}

static int list_directory_tool_handler(const cJSON *arguments, char **output_out) {
    const char *path = get_json_string(arguments, "path");
    if (!path) path = ".";

    DIR *dir = opendir(path);
    if (!dir) {
        char err_msg[512];
        snprintf(err_msg, sizeof(err_msg), "Error: Failed to open directory '%s': %s", path, strerror(errno));
        return set_output_string(output_out, err_msg);
    }

    struct string_buf buf = {NULL, 0};
    string_buf_append(&buf, "Directory listing for '", 23);
    string_buf_append(&buf, path, strlen(path));
    string_buf_append(&buf, "':\n", 3);

    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        const char *type = "UNKNOWN";
        if (entry->d_type == DT_DIR) type = "DIR";
        else if (entry->d_type == DT_REG) type = "FILE";
        else if (entry->d_type == DT_LNK) type = "LINK";

        char entry_line[1024];
        snprintf(entry_line, sizeof(entry_line), "  [%s] %s\n", type, entry->d_name);
        string_buf_append(&buf, entry_line, strlen(entry_line));
    }
    closedir(dir);

    if (!buf.data) {
        return set_output_string(output_out, "(empty directory)");
    }

    int res = set_output_string(output_out, buf.data);
    free(buf.data);
    return res;
}

static void find_files_recursive(const char *dir_path, const char *pattern, struct string_buf *buf) {
    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (strcmp(entry->d_name, ".git") == 0 || strcmp(entry->d_name, "build") == 0 || strcmp(entry->d_name, "node_modules") == 0) continue;

        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        if (strstr(entry->d_name, pattern) != NULL) {
            string_buf_append(buf, full_path, strlen(full_path));
            string_buf_append(buf, "\n", 1);
        }

        if (entry->d_type == DT_DIR) {
            find_files_recursive(full_path, pattern, buf);
        }
    }
    closedir(dir);
}

static int find_files_tool_handler(const cJSON *arguments, char **output_out) {
    const char *directory = get_json_string(arguments, "directory");
    if (!directory) directory = ".";
    const char *pattern = get_json_string(arguments, "pattern");
    if (!pattern) {
        return set_output_string(output_out, "Error: Missing required argument 'pattern'");
    }

    struct string_buf buf = {NULL, 0};
    find_files_recursive(directory, pattern, &buf);

    if (!buf.data || buf.size == 0) {
        char msg[512];
        snprintf(msg, sizeof(msg), "No files matching '%s' found in '%s'", pattern, directory);
        free(buf.data);
        return set_output_string(output_out, msg);
    }

    int res = set_output_string(output_out, buf.data);
    free(buf.data);
    return res;
}

static void grep_files_recursive(const char *dir_path, const char *query, struct string_buf *buf) {
    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (strcmp(entry->d_name, ".git") == 0 || strcmp(entry->d_name, "build") == 0 || strcmp(entry->d_name, "node_modules") == 0) continue;

        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        if (entry->d_type == DT_REG) {
            FILE *f = fopen(full_path, "r");
            if (f) {
                char line[2048];
                int line_num = 1;
                while (fgets(line, sizeof(line), f)) {
                    if (strstr(line, query) != NULL) {
                        char match_info[4096];
                        snprintf(match_info, sizeof(match_info), "%s:%d: %s", full_path, line_num, line);
                        string_buf_append(buf, match_info, strlen(match_info));
                    }
                    line_num++;
                }
                fclose(f);
            }
        } else if (entry->d_type == DT_DIR) {
            grep_files_recursive(full_path, query, buf);
        }
    }
    closedir(dir);
}

static int file_search_tool_handler(const cJSON *arguments, char **output_out) {
    const char *directory = get_json_string(arguments, "directory");
    if (!directory) directory = ".";
    const char *query = get_json_string(arguments, "query");
    if (!query) {
        return set_output_string(output_out, "Error: Missing required argument 'query'");
    }

    struct string_buf buf = {NULL, 0};
    grep_files_recursive(directory, query, &buf);

    if (!buf.data || buf.size == 0) {
        char msg[512];
        snprintf(msg, sizeof(msg), "No matches found for '%s' in '%s'", query, directory);
        free(buf.data);
        return set_output_string(output_out, msg);
    }

    int res = set_output_string(output_out, buf.data);
    free(buf.data);
    return res;
}

static int sys_info_tool_handler(const cJSON *arguments, char **output_out) {
    (void)arguments;
    struct utsname uts;
    char system_details[2048];

    if (uname(&uts) != 0) {
        snprintf(uts.sysname, sizeof(uts.sysname), "Unknown");
        snprintf(uts.release, sizeof(uts.release), "Unknown");
        snprintf(uts.machine, sizeof(uts.machine), "Unknown");
    }

    char cwd[512];
    if (!getcwd(cwd, sizeof(cwd))) {
        snprintf(cwd, sizeof(cwd), "Unknown");
    }

    const char *user = getenv("USER");
    if (!user) user = getenv("LOGNAME");
    if (!user) user = "Unknown";

    const char *shell = getenv("SHELL");
    if (!shell) shell = "Unknown";

    snprintf(system_details, sizeof(system_details),
             "OS: %s\nRelease: %s\nArchitecture: %s\nCwd: %s\nUser: %s\nShell: %s\n",
             uts.sysname, uts.release, uts.machine, cwd, user, shell);

    return set_output_string(output_out, system_details);
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

static cJSON *build_list_directory_tool_schema(void) {
    cJSON *parameters = cJSON_CreateObject();
    cJSON *properties = cJSON_CreateObject();
    cJSON *path = cJSON_CreateObject();

    if (!parameters || !properties || !path) {
        cJSON_Delete(parameters);
        cJSON_Delete(properties);
        cJSON_Delete(path);
        return NULL;
    }

    cJSON_AddStringToObject(parameters, "type", "object");
    cJSON_AddItemToObject(parameters, "properties", properties);
    cJSON_AddBoolToObject(parameters, "additionalProperties", 0);

    cJSON_AddStringToObject(path, "type", "string");
    cJSON_AddStringToObject(path, "description", "Path to the directory to list (defaults to '.')");
    cJSON_AddItemToObject(properties, "path", path);

    return build_tool_schema("ListDirectory", "List files and folders in a directory", parameters);
}

static cJSON *build_find_files_tool_schema(void) {
    cJSON *parameters = cJSON_CreateObject();
    cJSON *properties = cJSON_CreateObject();
    cJSON *required = cJSON_CreateArray();
    cJSON *directory = cJSON_CreateObject();
    cJSON *pattern = cJSON_CreateObject();

    if (!parameters || !properties || !required || !directory || !pattern) {
        cJSON_Delete(parameters);
        cJSON_Delete(properties);
        cJSON_Delete(required);
        cJSON_Delete(directory);
        cJSON_Delete(pattern);
        return NULL;
    }

    cJSON_AddStringToObject(parameters, "type", "object");
    cJSON_AddItemToObject(parameters, "properties", properties);
    cJSON_AddItemToObject(parameters, "required", required);
    cJSON_AddBoolToObject(parameters, "additionalProperties", 0);

    cJSON_AddStringToObject(directory, "type", "string");
    cJSON_AddStringToObject(directory, "description", "Directory path to start searching in (defaults to '.')");
    cJSON_AddItemToObject(properties, "directory", directory);

    cJSON_AddStringToObject(pattern, "type", "string");
    cJSON_AddStringToObject(pattern, "description", "Substring or glob pattern to match against file names");
    cJSON_AddItemToObject(properties, "pattern", pattern);

    cJSON_AddItemToArray(required, cJSON_CreateString("pattern"));

    return build_tool_schema("FindFiles", "Recursively find files matching a name pattern", parameters);
}

static cJSON *build_file_search_tool_schema(void) {
    cJSON *parameters = cJSON_CreateObject();
    cJSON *properties = cJSON_CreateObject();
    cJSON *required = cJSON_CreateArray();
    cJSON *directory = cJSON_CreateObject();
    cJSON *query = cJSON_CreateObject();

    if (!parameters || !properties || !required || !directory || !query) {
        cJSON_Delete(parameters);
        cJSON_Delete(properties);
        cJSON_Delete(required);
        cJSON_Delete(directory);
        cJSON_Delete(query);
        return NULL;
    }

    cJSON_AddStringToObject(parameters, "type", "object");
    cJSON_AddItemToObject(parameters, "properties", properties);
    cJSON_AddItemToObject(parameters, "required", required);
    cJSON_AddBoolToObject(parameters, "additionalProperties", 0);

    cJSON_AddStringToObject(directory, "type", "string");
    cJSON_AddStringToObject(directory, "description", "Directory path to search in (defaults to '.')");
    cJSON_AddItemToObject(properties, "directory", directory);

    cJSON_AddStringToObject(query, "type", "string");
    cJSON_AddStringToObject(query, "description", "Text pattern or substring to search for inside files");
    cJSON_AddItemToObject(properties, "query", query);

    cJSON_AddItemToArray(required, cJSON_CreateString("query"));

    return build_tool_schema("FileSearch", "Recursively search for text patterns inside files (like grep)", parameters);
}

static cJSON *build_sys_info_tool_schema(void) {
    cJSON *parameters = cJSON_CreateObject();
    cJSON *properties = cJSON_CreateObject();
    if (!parameters || !properties) {
        cJSON_Delete(parameters);
        cJSON_Delete(properties);
        return NULL;
    }
    cJSON_AddStringToObject(parameters, "type", "object");
    cJSON_AddItemToObject(parameters, "properties", properties);
    cJSON_AddBoolToObject(parameters, "additionalProperties", 0);

    return build_tool_schema("SysInfo", "Get current system details (OS, directory, user, shell)", parameters);
}

static cJSON *build_create_file_tool_schema(void) {
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
    cJSON_AddStringToObject(file_path, "description", "Path to the file to create");
    cJSON_AddItemToObject(properties, "file_path", file_path);
    cJSON_AddItemToArray(required, cJSON_CreateString("file_path"));

    return build_tool_schema("CreateFile", "Create a new empty file at the specified path", parameters);
}

static cJSON *build_create_directory_tool_schema(void) {
    cJSON *parameters = cJSON_CreateObject();
    cJSON *properties = cJSON_CreateObject();
    cJSON *required = cJSON_CreateArray();
    cJSON *directory_path = cJSON_CreateObject();

    if (!parameters || !properties || !required || !directory_path) {
        cJSON_Delete(parameters);
        cJSON_Delete(properties);
        cJSON_Delete(required);
        cJSON_Delete(directory_path);
        return NULL;
    }

    cJSON_AddStringToObject(parameters, "type", "object");
    cJSON_AddItemToObject(parameters, "properties", properties);
    cJSON_AddItemToObject(parameters, "required", required);
    cJSON_AddBoolToObject(parameters, "additionalProperties", 0);

    cJSON_AddStringToObject(directory_path, "type", "string");
    cJSON_AddStringToObject(directory_path, "description", "Path to the directory to create");
    cJSON_AddItemToObject(properties, "directory_path", directory_path);
    cJSON_AddItemToArray(required, cJSON_CreateString("directory_path"));

    return build_tool_schema("CreateDirectory", "Create a new directory (and intermediate folders) at the specified path", parameters);
}

cJSON *tool_registry_build_schema(void) {
    cJSON *tools = cJSON_CreateArray();
    if (!tools) return NULL;

    cJSON *t1 = build_read_tool_schema();
    cJSON *t2 = build_write_tool_schema();
    cJSON *t3 = build_bash_tool_schema();
    cJSON *t4 = build_list_directory_tool_schema();
    cJSON *t5 = build_find_files_tool_schema();
    cJSON *t6 = build_file_search_tool_schema();
    cJSON *t7 = build_sys_info_tool_schema();
    cJSON *t8 = build_create_file_tool_schema();
    cJSON *t9 = build_create_directory_tool_schema();

    if (!t1 || !t2 || !t3 || !t4 || !t5 || !t6 || !t7 || !t8 || !t9) {
        cJSON_Delete(tools);
        cJSON_Delete(t1);
        cJSON_Delete(t2);
        cJSON_Delete(t3);
        cJSON_Delete(t4);
        cJSON_Delete(t5);
        cJSON_Delete(t6);
        cJSON_Delete(t7);
        cJSON_Delete(t8);
        cJSON_Delete(t9);
        return NULL;
    }

    cJSON_AddItemToArray(tools, t1);
    cJSON_AddItemToArray(tools, t2);
    cJSON_AddItemToArray(tools, t3);
    cJSON_AddItemToArray(tools, t4);
    cJSON_AddItemToArray(tools, t5);
    cJSON_AddItemToArray(tools, t6);
    cJSON_AddItemToArray(tools, t7);
    cJSON_AddItemToArray(tools, t8);
    cJSON_AddItemToArray(tools, t9);

    return tools;
}

int tool_registry_execute(const cJSON *tool_call, char **output_out) {
    static const struct tool_handler handlers[] = {
        {"Read", read_tool_handler},
        {"Write", write_tool_handler},
        {"Bash", bash_tool_handler},
        {"ListDirectory", list_directory_tool_handler},
        {"FindFiles", find_files_tool_handler},
        {"FileSearch", file_search_tool_handler},
        {"SysInfo", sys_info_tool_handler},
        {"CreateFile", create_file_tool_handler},
        {"CreateDirectory", create_directory_tool_handler},
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

    cJSON *arguments = NULL;
    if (arguments_str) {
        arguments = cJSON_Parse(arguments_str);
        if (!arguments) {
            fprintf(stderr, "failed to parse arguments for tool %s\n", name);
            return 1;
        }
    } else {
        arguments = cJSON_CreateObject();
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
