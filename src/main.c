#include <curl/curl.h>
#include <cjson/cJSON.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct response_buf {
    char *data;
    size_t size;
};

typedef int (*tool_handler_fn)(const cJSON *arguments);

struct tool_handler {
    const char *name;
    tool_handler_fn handler;
};

static size_t curl_write_response(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    struct response_buf *buf = (struct response_buf *)userp;
    char *tmp = realloc(buf->data, buf->size + total + 1);
    if (!tmp) return 0;

    buf->data = tmp;
    memcpy(buf->data + buf->size, contents, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
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

static int print_file_contents(const char *path) {
    size_t size = 0;
    char *contents = read_entire_file(path, &size);
    if (!contents) return 1;

    if (size > 0) {
        fwrite(contents, 1, size, stdout);
    }

    free(contents);
    return 0;
}

static const char *get_json_string(const cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL) return NULL;
    return item->valuestring;
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

static cJSON *build_request(const char *prompt) {
    cJSON *req = cJSON_CreateObject();
    cJSON *messages = cJSON_CreateArray();
    cJSON *tools = cJSON_CreateArray();
    cJSON *user_message = cJSON_CreateObject();
    cJSON *tool = build_read_tool_schema();

    if (!req || !messages || !tools || !user_message || !tool) {
        cJSON_Delete(req);
        cJSON_Delete(messages);
        cJSON_Delete(tools);
        cJSON_Delete(user_message);
        cJSON_Delete(tool);
        return NULL;
    }

    cJSON_AddStringToObject(req, "model", "anthropic/claude-haiku-4.5");
    cJSON_AddItemToObject(req, "messages", messages);
    cJSON_AddItemToObject(req, "tools", tools);

    cJSON_AddStringToObject(user_message, "role", "user");
    cJSON_AddStringToObject(user_message, "content", prompt);
    cJSON_AddItemToArray(messages, user_message);
    cJSON_AddItemToArray(tools, tool);

    return req;
}

static int perform_chat_completion(const char *base_url, const char *api_key, const char *body, struct response_buf *resp) {
    char url[512];
    char auth_header[512];

    snprintf(url, sizeof(url), "%s/chat/completions", base_url);
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);

    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "failed to initialize curl\n");
        return 1;
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth_header);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);

    CURLcode res = curl_easy_perform(curl);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "curl error: %s\n", curl_easy_strerror(res));
        return 1;
    }

    return 0;
}

static int handle_read_tool_call(const cJSON *arguments) {
    const char *file_path = get_json_string(arguments, "file_path");
    if (!file_path) {
        fprintf(stderr, "Read tool call is missing file_path\n");
        return 1;
    }

    return print_file_contents(file_path);
}

static int dispatch_tool_call(const cJSON *tool_call) {
    static const struct tool_handler handlers[] = {
        {"Read", handle_read_tool_call},
    };

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

    int result = 1;
    size_t handler_count = sizeof(handlers) / sizeof(handlers[0]);
    for (size_t i = 0; i < handler_count; ++i) {
        if (strcmp(name, handlers[i].name) == 0) {
            result = handlers[i].handler(arguments);
            cJSON_Delete(arguments);
            return result;
        }
    }

    fprintf(stderr, "unsupported tool: %s\n", name);
    cJSON_Delete(arguments);
    return result;
}

static int handle_response(const char *json_text) {
    cJSON *json = cJSON_Parse(json_text);
    if (!json) {
        fprintf(stderr, "failed to parse response JSON\n");
        return 1;
    }

    cJSON *choices = cJSON_GetObjectItemCaseSensitive(json, "choices");
    if (!cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        fprintf(stderr, "no choices in response\n");
        cJSON_Delete(json);
        return 1;
    }

    cJSON *first_choice = cJSON_GetArrayItem(choices, 0);
    cJSON *message = cJSON_GetObjectItemCaseSensitive(first_choice, "message");
    if (!cJSON_IsObject(message)) {
        fprintf(stderr, "missing assistant message\n");
        cJSON_Delete(json);
        return 1;
    }

    cJSON *tool_calls = cJSON_GetObjectItemCaseSensitive(message, "tool_calls");
    if (cJSON_IsArray(tool_calls) && cJSON_GetArraySize(tool_calls) > 0) {
        cJSON *first_tool_call = cJSON_GetArrayItem(tool_calls, 0);
        int result = dispatch_tool_call(first_tool_call);
        cJSON_Delete(json);
        return result;
    }

    cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
    if (cJSON_IsString(content) && content->valuestring != NULL) {
        fputs(content->valuestring, stdout);
    }

    cJSON_Delete(json);
    return 0;
}

int main(int argc, char *argv[]) {
    const char *prompt = NULL;
    int opt;

    while ((opt = getopt(argc, argv, "p:")) != -1) {
        switch (opt) {
            case 'p':
                prompt = optarg;
                break;
            default:
                fprintf(stderr, "usage: %s -p <prompt>\n", argv[0]);
                return 1;
        }
    }

    if (!prompt) {
        fprintf(stderr, "error: -p flag is required\n");
        return 1;
    }

    const char *api_key = getenv("OPENROUTER_API_KEY");
    const char *base_url = getenv("OPENROUTER_BASE_URL");
    if (!base_url || !*base_url) base_url = "https://openrouter.ai/api/v1";
    if (!api_key || !*api_key) {
        fprintf(stderr, "OPENROUTER_API_KEY is not set\n");
        return 1;
    }

    cJSON *request = build_request(prompt);
    if (!request) {
        fprintf(stderr, "failed to build request\n");
        return 1;
    }

    char *request_body = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (!request_body) {
        fprintf(stderr, "failed to serialize request\n");
        return 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    struct response_buf resp = {NULL, 0};
    int status = perform_chat_completion(base_url, api_key, request_body, &resp);
    free(request_body);

    if (status == 0) {
        status = handle_response(resp.data ? resp.data : "");
    }

    free(resp.data);
    curl_global_cleanup();
    return status;
}
