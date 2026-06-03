#include "app.h"

#include "openrouter.h"
#include "tool_registry.h"

#include <curl/curl.h>
#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cJSON *get_first_choice_message(cJSON *response) {
    cJSON *choices = cJSON_GetObjectItemCaseSensitive(response, "choices");
    if (!cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        return NULL;
    }

    cJSON *first_choice = cJSON_GetArrayItem(choices, 0);
    cJSON *message = cJSON_GetObjectItemCaseSensitive(first_choice, "message");
    if (!cJSON_IsObject(message)) {
        return NULL;
    }

    return message;
}

static cJSON *build_request(const char *prompt) {
    cJSON *request = cJSON_CreateObject();
    cJSON *messages = cJSON_CreateArray();
    cJSON *tools = tool_registry_build_schema();
    cJSON *user_message = cJSON_CreateObject();

    if (!request || !messages || !tools || !user_message) {
        cJSON_Delete(request);
        cJSON_Delete(messages);
        cJSON_Delete(tools);
        cJSON_Delete(user_message);
        return NULL;
    }

    cJSON_AddStringToObject(request, "model", "anthropic/claude-haiku-4.5");
    cJSON_AddItemToObject(request, "messages", messages);
    cJSON_AddItemToObject(request, "tools", tools);

    cJSON_AddStringToObject(user_message, "role", "user");
    cJSON_AddStringToObject(user_message, "content", prompt);
    cJSON_AddItemToArray(messages, user_message);

    return request;
}

static int append_tool_result_message(cJSON *messages, const cJSON *tool_call, const char *content) {
    cJSON *tool_message = cJSON_CreateObject();
    if (!tool_message) return 1;

    cJSON *tool_call_id_item = cJSON_GetObjectItemCaseSensitive((cJSON *)tool_call, "id");
    const char *tool_call_id = cJSON_IsString(tool_call_id_item) ? tool_call_id_item->valuestring : NULL;

    cJSON_AddStringToObject(tool_message, "role", "tool");
    cJSON_AddStringToObject(tool_message, "tool_call_id", tool_call_id ? tool_call_id : "");
    cJSON_AddStringToObject(tool_message, "content", content ? content : "");
    cJSON_AddItemToArray(messages, tool_message);
    return 0;
}

static int handle_tool_calls(cJSON *messages, cJSON *tool_calls) {
    size_t count = cJSON_GetArraySize(tool_calls);
    for (size_t i = 0; i < count; ++i) {
        cJSON *tool_call = cJSON_GetArrayItem(tool_calls, (int)i);
        char *tool_output = NULL;

        if (tool_registry_execute(tool_call, &tool_output) != 0) {
            free(tool_output);
            return 1;
        }

        if (append_tool_result_message(messages, tool_call, tool_output ? tool_output : "") != 0) {
            free(tool_output);
            return 1;
        }

        free(tool_output);
    }

    return 0;
}

int app_run(const char *prompt, const char *api_key, const char *base_url) {
    if (!prompt || !api_key || !base_url) {
        return 1;
    }

    cJSON *request = build_request(prompt);
    if (!request) {
        fprintf(stderr, "failed to build request\n");
        return 1;
    }

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        fprintf(stderr, "failed to initialize curl globally\n");
        cJSON_Delete(request);
        return 1;
    }

    int status = 0;
    const int max_iterations = 16;

    for (int iteration = 0; iteration < max_iterations; ++iteration) {
        cJSON *response = NULL;
        status = openrouter_chat_completion(base_url, api_key, request, &response);
        if (status != 0) {
            break;
        }

        cJSON *message = get_first_choice_message(response);
        if (!message) {
            fprintf(stderr, "missing assistant message\n");
            cJSON_Delete(response);
            status = 1;
            break;
        }

        cJSON *tool_calls = cJSON_GetObjectItemCaseSensitive(message, "tool_calls");
        if (cJSON_IsArray(tool_calls) && cJSON_GetArraySize(tool_calls) > 0) {
            cJSON *messages = cJSON_GetObjectItemCaseSensitive(request, "messages");
            cJSON *assistant_message = cJSON_Duplicate(message, 1);
            if (!cJSON_IsArray(messages) || !assistant_message) {
                cJSON_Delete(assistant_message);
                cJSON_Delete(response);
                status = 1;
                break;
            }

            cJSON_AddItemToArray(messages, assistant_message);
            status = handle_tool_calls(messages, tool_calls);
            cJSON_Delete(response);
            if (status != 0) break;
            continue;
        }

        cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
        if (cJSON_IsString(content) && content->valuestring != NULL) {
            fputs(content->valuestring, stdout);
        }

        cJSON_Delete(response);
        status = 0;
        break;
    }

    cJSON_Delete(request);
    curl_global_cleanup();
    return status;
}
