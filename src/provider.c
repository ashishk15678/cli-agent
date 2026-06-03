#include "provider.h"
#include "utils.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct response_buf {
    char *data;
    size_t size;
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

cJSON *translate_to_anthropic(const cJSON *openai_req) {
    cJSON *anthropic_req = cJSON_CreateObject();
    if (!anthropic_req) return NULL;

    cJSON *model_item = cJSON_GetObjectItemCaseSensitive((cJSON *)openai_req, "model");
    if (cJSON_IsString(model_item)) {
        cJSON_AddStringToObject(anthropic_req, "model", model_item->valuestring);
    }

    cJSON_AddNumberToObject(anthropic_req, "max_tokens", 4096);

    cJSON *system_prompt_buf = cJSON_CreateString("");
    cJSON *messages_arr = cJSON_GetObjectItemCaseSensitive((cJSON *)openai_req, "messages");
    cJSON *anthropic_messages = cJSON_CreateArray();

    if (cJSON_IsArray(messages_arr)) {
        int msg_count = cJSON_GetArraySize(messages_arr);
        cJSON *current_tool_user_msg = NULL;
        cJSON *current_tool_content_arr = NULL;

        for (int i = 0; i < msg_count; i++) {
            cJSON *msg = cJSON_GetArrayItem(messages_arr, i);
            cJSON *role_item = cJSON_GetObjectItemCaseSensitive(msg, "role");
            if (!cJSON_IsString(role_item)) continue;

            const char *role = role_item->valuestring;
            cJSON *content_item = cJSON_GetObjectItemCaseSensitive(msg, "content");

            if (strcmp(role, "system") == 0) {
                if (cJSON_IsString(content_item)) {
                    const char *prev = system_prompt_buf->valuestring;
                    size_t new_len = strlen(prev) + strlen(content_item->valuestring) + 2;
                    char *combined = malloc(new_len);
                    if (combined) {
                        if (strlen(prev) > 0) {
                            snprintf(combined, new_len, "%s\n%s", prev, content_item->valuestring);
                        } else {
                            snprintf(combined, new_len, "%s", content_item->valuestring);
                        }
                        cJSON_SetValuestring(system_prompt_buf, combined);
                        free(combined);
                    }
                }
                current_tool_user_msg = NULL;
                current_tool_content_arr = NULL;
            } else if (strcmp(role, "user") == 0) {
                cJSON *anthropic_msg = cJSON_CreateObject();
                cJSON_AddStringToObject(anthropic_msg, "role", "user");
                if (cJSON_IsString(content_item)) {
                    cJSON_AddStringToObject(anthropic_msg, "content", content_item->valuestring);
                }
                cJSON_AddItemToArray(anthropic_messages, anthropic_msg);
                current_tool_user_msg = NULL;
                current_tool_content_arr = NULL;
            } else if (strcmp(role, "assistant") == 0) {
                cJSON *anthropic_msg = cJSON_CreateObject();
                cJSON_AddStringToObject(anthropic_msg, "role", "assistant");

                cJSON *tool_calls = cJSON_GetObjectItemCaseSensitive(msg, "tool_calls");
                if (cJSON_IsArray(tool_calls) && cJSON_GetArraySize(tool_calls) > 0) {
                    cJSON *content_arr = cJSON_CreateArray();

                    if (cJSON_IsString(content_item) && strlen(content_item->valuestring) > 0) {
                        cJSON *text_block = cJSON_CreateObject();
                        cJSON_AddStringToObject(text_block, "type", "text");
                        cJSON_AddStringToObject(text_block, "text", content_item->valuestring);
                        cJSON_AddItemToArray(content_arr, text_block);
                    }

                    int tc_count = cJSON_GetArraySize(tool_calls);
                    for (int j = 0; j < tc_count; j++) {
                        cJSON *tc = cJSON_GetArrayItem(tool_calls, j);
                        cJSON *id_item = cJSON_GetObjectItemCaseSensitive(tc, "id");
                        cJSON *func_item = cJSON_GetObjectItemCaseSensitive(tc, "function");
                        if (!cJSON_IsObject(func_item)) continue;

                        cJSON *name_item = cJSON_GetObjectItemCaseSensitive(func_item, "name");
                        cJSON *args_item = cJSON_GetObjectItemCaseSensitive(func_item, "arguments");

                        cJSON *tool_use_block = cJSON_CreateObject();
                        cJSON_AddStringToObject(tool_use_block, "type", "tool_use");
                        cJSON_AddStringToObject(tool_use_block, "id", cJSON_IsString(id_item) ? id_item->valuestring : "");
                        cJSON_AddStringToObject(tool_use_block, "name", cJSON_IsString(name_item) ? name_item->valuestring : "");

                        if (cJSON_IsString(args_item)) {
                            cJSON *input_obj = cJSON_Parse(args_item->valuestring);
                            if (input_obj) {
                                cJSON_AddItemToObject(tool_use_block, "input", input_obj);
                            } else {
                                cJSON_AddItemToObject(tool_use_block, "input", cJSON_CreateObject());
                            }
                        } else {
                            cJSON_AddItemToObject(tool_use_block, "input", cJSON_CreateObject());
                        }

                        cJSON_AddItemToArray(content_arr, tool_use_block);
                    }
                    cJSON_AddItemToObject(anthropic_msg, "content", content_arr);
                } else {
                    if (cJSON_IsString(content_item)) {
                        cJSON_AddStringToObject(anthropic_msg, "content", content_item->valuestring);
                    } else {
                        cJSON_AddStringToObject(anthropic_msg, "content", "");
                    }
                }
                cJSON_AddItemToArray(anthropic_messages, anthropic_msg);
                current_tool_user_msg = NULL;
                current_tool_content_arr = NULL;
            } else if (strcmp(role, "tool") == 0) {
                cJSON *tool_call_id_item = cJSON_GetObjectItemCaseSensitive(msg, "tool_call_id");
                const char *tool_call_id = cJSON_IsString(tool_call_id_item) ? tool_call_id_item->valuestring : "";
                const char *content_str = cJSON_IsString(content_item) ? content_item->valuestring : "";

                cJSON *tool_result_block = cJSON_CreateObject();
                cJSON_AddStringToObject(tool_result_block, "type", "tool_result");
                cJSON_AddStringToObject(tool_result_block, "tool_use_id", tool_call_id);
                cJSON_AddStringToObject(tool_result_block, "content", content_str);

                if (current_tool_user_msg == NULL) {
                    current_tool_user_msg = cJSON_CreateObject();
                    cJSON_AddStringToObject(current_tool_user_msg, "role", "user");
                    current_tool_content_arr = cJSON_CreateArray();
                    cJSON_AddItemToObject(current_tool_user_msg, "content", current_tool_content_arr);
                    cJSON_AddItemToArray(anthropic_messages, current_tool_user_msg);
                }

                cJSON_AddItemToArray(current_tool_content_arr, tool_result_block);
            }
        }
    }

    if (strlen(system_prompt_buf->valuestring) > 0) {
        cJSON_AddItemToObject(anthropic_req, "system", system_prompt_buf);
    } else {
        cJSON_Delete(system_prompt_buf);
    }

    cJSON_AddItemToObject(anthropic_req, "messages", anthropic_messages);

    cJSON *tools_arr = cJSON_GetObjectItemCaseSensitive((cJSON *)openai_req, "tools");
    if (cJSON_IsArray(tools_arr) && cJSON_GetArraySize(tools_arr) > 0) {
        cJSON *anthropic_tools = cJSON_CreateArray();
        int tool_count = cJSON_GetArraySize(tools_arr);
        for (int i = 0; i < tool_count; i++) {
            cJSON *tool = cJSON_GetArrayItem(tools_arr, i);
            cJSON *func = cJSON_GetObjectItemCaseSensitive(tool, "function");
            if (!cJSON_IsObject(func)) continue;

            cJSON *name = cJSON_GetObjectItemCaseSensitive(func, "name");
            cJSON *description = cJSON_GetObjectItemCaseSensitive(func, "description");
            cJSON *params = cJSON_GetObjectItemCaseSensitive(func, "parameters");

            cJSON *anthropic_tool = cJSON_CreateObject();
            cJSON_AddStringToObject(anthropic_tool, "name", cJSON_IsString(name) ? name->valuestring : "");
            cJSON_AddStringToObject(anthropic_tool, "description", cJSON_IsString(description) ? description->valuestring : "");

            if (cJSON_IsObject(params)) {
                cJSON_AddItemToObject(anthropic_tool, "input_schema", cJSON_Duplicate(params, 1));
            } else {
                cJSON_AddItemToObject(anthropic_tool, "input_schema", cJSON_CreateObject());
            }

            cJSON_AddItemToArray(anthropic_tools, anthropic_tool);
        }
        cJSON_AddItemToObject(anthropic_req, "tools", anthropic_tools);
    }

    return anthropic_req;
}

cJSON *translate_from_anthropic(const cJSON *anthropic_resp) {
    cJSON *openai_resp = cJSON_CreateObject();
    if (!openai_resp) return NULL;

    cJSON *choices = cJSON_CreateArray();
    cJSON *choice = cJSON_CreateObject();
    cJSON *message = cJSON_CreateObject();

    cJSON_AddItemToObject(openai_resp, "choices", choices);
    cJSON_AddItemToArray(choices, choice);
    cJSON_AddItemToObject(choice, "message", message);
    cJSON_AddStringToObject(choice, "finish_reason", "stop");
    cJSON_AddNumberToObject(choice, "index", 0);

    cJSON_AddStringToObject(message, "role", "assistant");

    cJSON *content_item = cJSON_GetObjectItemCaseSensitive((cJSON *)anthropic_resp, "content");
    cJSON *tool_calls = cJSON_CreateArray();
    struct string_buf text_content = {NULL, 0};

    if (cJSON_IsArray(content_item)) {
        int content_count = cJSON_GetArraySize(content_item);
        for (int i = 0; i < content_count; i++) {
            cJSON *block = cJSON_GetArrayItem(content_item, i);
            cJSON *type_item = cJSON_GetObjectItemCaseSensitive(block, "type");
            if (!cJSON_IsString(type_item)) continue;

            if (strcmp(type_item->valuestring, "text") == 0) {
                cJSON *text_item = cJSON_GetObjectItemCaseSensitive(block, "text");
                if (cJSON_IsString(text_item)) {
                    string_buf_append(&text_content, text_item->valuestring, strlen(text_item->valuestring));
                }
            } else if (strcmp(type_item->valuestring, "tool_use") == 0) {
                cJSON *id_item = cJSON_GetObjectItemCaseSensitive(block, "id");
                cJSON *name_item = cJSON_GetObjectItemCaseSensitive(block, "name");
                cJSON *input_item = cJSON_GetObjectItemCaseSensitive(block, "input");

                cJSON *tool_call = cJSON_CreateObject();
                cJSON_AddStringToObject(tool_call, "id", cJSON_IsString(id_item) ? id_item->valuestring : "");
                cJSON_AddStringToObject(tool_call, "type", "function");

                cJSON *func = cJSON_CreateObject();
                cJSON_AddStringToObject(func, "name", cJSON_IsString(name_item) ? name_item->valuestring : "");

                if (cJSON_IsObject(input_item)) {
                    char *args_str = cJSON_PrintUnformatted(input_item);
                    if (args_str) {
                        cJSON_AddStringToObject(func, "arguments", args_str);
                        free(args_str);
                    } else {
                        cJSON_AddStringToObject(func, "arguments", "{}");
                    }
                } else {
                    cJSON_AddStringToObject(func, "arguments", "{}");
                }

                cJSON_AddItemToObject(tool_call, "function", func);
                cJSON_AddItemToArray(tool_calls, tool_call);
            }
        }
    } else if (cJSON_IsString(content_item)) {
        string_buf_append(&text_content, content_item->valuestring, strlen(content_item->valuestring));
    }

    if (text_content.data) {
        cJSON_AddStringToObject(message, "content", text_content.data);
        free(text_content.data);
    } else {
        cJSON_AddStringToObject(message, "content", "");
    }

    if (cJSON_GetArraySize(tool_calls) > 0) {
        cJSON_AddItemToObject(message, "tool_calls", tool_calls);
        cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(choice, "finish_reason"), "tool_calls");
    } else {
        cJSON_Delete(tool_calls);
    }

    return openai_resp;
}

int provider_chat_completion(const agent_config_t *config, const cJSON *request, cJSON **response_out) {
    if (!config || !request || !response_out) return 1;

    cJSON *payload = NULL;
    char url[1024];

    if (config->provider == PROVIDER_ANTHROPIC) {
        payload = translate_to_anthropic(request);
        snprintf(url, sizeof(url), "%s/messages", config->base_url);
    } else {
        payload = (cJSON *)request;
        snprintf(url, sizeof(url), "%s/chat/completions", config->base_url);
    }

    if (!payload) {
        fprintf(stderr, "failed to prepare payload for provider\n");
        return 1;
    }

    char *body = cJSON_PrintUnformatted(payload);
    if (config->provider == PROVIDER_ANTHROPIC) {
        cJSON_Delete(payload);
    }

    if (!body) {
        fprintf(stderr, "failed to serialize request body\n");
        return 1;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "failed to initialize curl\n");
        free(body);
        return 1;
    }

    struct response_buf resp = {NULL, 0};
    struct curl_slist *headers = NULL;

    if (config->provider == PROVIDER_ANTHROPIC) {
        headers = curl_slist_append(headers, "content-type: application/json");
        char auth_hdr[512];
        snprintf(auth_hdr, sizeof(auth_hdr), "x-api-key: %s", config->api_key ? config->api_key : "");
        headers = curl_slist_append(headers, auth_hdr);
        headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");
    } else {
        headers = curl_slist_append(headers, "Content-Type: application/json");
        if (config->api_key && strlen(config->api_key) > 0) {
            char auth_hdr[512];
            snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", config->api_key);
            headers = curl_slist_append(headers, auth_hdr);
            if (config->provider == PROVIDER_GEMINI) {
                char goog_hdr[512];
                snprintf(goog_hdr, sizeof(goog_hdr), "x-goog-api-key: %s", config->api_key);
                headers = curl_slist_append(headers, goog_hdr);
            }
        }
        if (config->provider == PROVIDER_OPENROUTER) {
            headers = curl_slist_append(headers, "HTTP-Referer: https://github.com/codecrafters-io/claude-code-c");
            headers = curl_slist_append(headers, "X-Title: Claude Code C");
        }
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

    CURLcode res = curl_easy_perform(curl);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body);

    if (res != CURLE_OK) {
        fprintf(stderr, "curl request error: %s\n", curl_easy_strerror(res));
        free(resp.data);
        return 1;
    }

    cJSON *parsed_resp = cJSON_Parse(resp.data ? resp.data : "");
    free(resp.data);

    if (!parsed_resp) {
        fprintf(stderr, "failed to parse provider response JSON\n");
        return 1;
    }

    cJSON *error_obj = cJSON_GetObjectItemCaseSensitive(parsed_resp, "error");
    if (cJSON_IsObject(error_obj)) {
        cJSON *msg_item = cJSON_GetObjectItemCaseSensitive(error_obj, "message");
        cJSON *failed_gen = cJSON_GetObjectItemCaseSensitive(error_obj, "failed_generation");
        if (cJSON_IsString(failed_gen)) {
            fprintf(stderr, "API Error: %s\nDetails (failed generation): %s\n", 
                    cJSON_IsString(msg_item) ? msg_item->valuestring : "Unknown API error",
                    failed_gen->valuestring);
        } else {
            fprintf(stderr, "API Error: %s\n", cJSON_IsString(msg_item) ? msg_item->valuestring : "Unknown API error");
        }
        cJSON_Delete(parsed_resp);
        return 1;
    }

    if (config->provider == PROVIDER_ANTHROPIC) {
        *response_out = translate_from_anthropic(parsed_resp);
        cJSON_Delete(parsed_resp);
        if (!*response_out) {
            fprintf(stderr, "failed to translate Anthropic response to OpenAI format\n");
            return 1;
        }
    } else {
        *response_out = parsed_resp;
    }

    return 0;
}
