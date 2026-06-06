#include "app.h"
#include "provider.h"
#include "tool_registry.h"
#include "utils.h"
#include "dropdown.h"
#include "memory.h"
#include "speculate.h"

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
            char err_buf[512];
            snprintf(err_buf, sizeof(err_buf), "Tool execution failed: %s", tool_output ? tool_output : "Unknown error");
            append_tool_result_message(messages, tool_call, err_buf);
            free(tool_output);
            continue;
        }

        printf("[Tool Result] Success (%zu bytes)\n", tool_output ? strlen(tool_output) : 0);
        fflush(stdout);

        if (append_tool_result_message(messages, tool_call, tool_output ? tool_output : "") != 0) {
            free(tool_output);
            return 1;
        }

        free(tool_output);
    }

    return 0;
}

static cJSON *try_parse_xml_tool_call(const char *text) {
    if (!text) return NULL;

    const char *start = strstr(text, "<function");
    if (!start) start = strstr(text, "<tool_call");
    if (!start) return NULL;

    const char *name_start = NULL;
    const char *args_start = NULL;
    char name[128] = {0};

    if (start[9] == '=') {
        name_start = start + 10;
        const char *name_end = strchr(name_start, '>');
        if (name_end) {
            size_t name_len = name_end - name_start;
            while (name_len > 0 && (name_start[name_len - 1] == ' ' || name_start[name_len - 1] == '\t' || name_start[name_len - 1] == '\n' || name_start[name_len - 1] == '\r')) {
                name_len--;
            }
            if (name_len < sizeof(name)) {
                memcpy(name, name_start, name_len);
                name[name_len] = '\0';
            }
            args_start = name_end + 1;
        }
    } else if (start[9] == '/') {
        name_start = start + 10;
        const char *name_end = strchr(name_start, '{');
        if (name_end) {
            size_t name_len = name_end - name_start;
            while (name_len > 0 && (name_start[name_len - 1] == ' ' || name_start[name_len - 1] == '\t' || name_start[name_len - 1] == '\n' || name_start[name_len - 1] == '\r')) {
                name_len--;
            }
            if (name_len < sizeof(name)) {
                memcpy(name, name_start, name_len);
                name[name_len] = '\0';
            }
            args_start = name_end;
        }
    }

    if (strlen(name) == 0 || !args_start) return NULL;

    const char *end = strstr(args_start, "</function>");
    if (!end) end = strstr(args_start, "</tool_call>");
    if (!end) return NULL;

    size_t args_len = end - args_start;
    char *args = malloc(args_len + 1);
    if (!args) return NULL;
    memcpy(args, args_start, args_len);
    args[args_len] = '\0';

    cJSON *tool_calls = cJSON_CreateArray();
    if (!tool_calls) {
        free(args);
        return NULL;
    }
    cJSON *tool_call = cJSON_CreateObject();
    if (!tool_call) {
        cJSON_Delete(tool_calls);
        free(args);
        return NULL;
    }
    cJSON_AddStringToObject(tool_call, "id", "call_fallback");
    cJSON_AddStringToObject(tool_call, "type", "function");

    cJSON *func = cJSON_CreateObject();
    if (!func) {
        cJSON_Delete(tool_calls);
        cJSON_Delete(tool_call);
        free(args);
        return NULL;
    }
    cJSON_AddStringToObject(func, "name", name);
    cJSON_AddStringToObject(func, "arguments", args);
    cJSON_AddItemToObject(tool_call, "function", func);
    cJSON_AddItemToArray(tool_calls, tool_call);

    free(args);
    return tool_calls;
}

static int run_agent_loop(const agent_config_t *config, cJSON *messages) {
    const int max_iterations = 20;
    int status = 0;

    cJSON *tools = tool_registry_build_schema();
    if (!tools) {
        fprintf(stderr, "failed to build tool schema\n");
        return 1;
    }

    cJSON *request = cJSON_CreateObject();
    if (!request) {
        cJSON_Delete(tools);
        return 1;
    }
    cJSON_AddStringToObject(request, "model", config->model);
    cJSON_AddItemToObject(request, "tools", tools);

    for (int iteration = 0; iteration < max_iterations; ++iteration) {
        cJSON_AddItemToObject(request, "messages", messages);

        cJSON *response = NULL;
        status = provider_chat_completion(config, request, &response);

        cJSON_DetachItemFromObject(request, "messages");

        if (status != 0) {
            break;
        }

        cJSON *message = get_first_choice_message(response);
        if (!message) {
            fprintf(stderr, "error: missing assistant message in response\n");
            cJSON_Delete(response);
            status = 1;
            break;
        }

        cJSON *tool_calls = cJSON_GetObjectItemCaseSensitive(message, "tool_calls");
        cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
        cJSON *fallback_tool_calls = NULL;
        int has_fallback = 0;

        if ((!tool_calls || cJSON_GetArraySize(tool_calls) == 0) && cJSON_IsString(content) && content->valuestring) {
            fallback_tool_calls = try_parse_xml_tool_call(content->valuestring);
            if (fallback_tool_calls) {
                tool_calls = fallback_tool_calls;
                has_fallback = 1;
            }
        }

        if (!has_fallback && cJSON_IsString(content) && content->valuestring && strlen(content->valuestring) > 0) {
            printf("%s\n", content->valuestring);
            fflush(stdout);
        }

        if (cJSON_IsArray(tool_calls) && cJSON_GetArraySize(tool_calls) > 0) {
            cJSON *assistant_msg_copy = cJSON_Duplicate(message, 1);
            if (has_fallback) {
                cJSON_AddItemToObject(assistant_msg_copy, "tool_calls", cJSON_Duplicate(fallback_tool_calls, 1));
            }
            cJSON_AddItemToArray(messages, assistant_msg_copy);

            int tc_count = cJSON_GetArraySize(tool_calls);
            for (int i = 0; i < tc_count; ++i) {
                cJSON *tool_call = cJSON_GetArrayItem(tool_calls, i);
                cJSON *func = cJSON_GetObjectItemCaseSensitive(tool_call, "function");
                cJSON *name = cJSON_GetObjectItemCaseSensitive(func, "name");
                cJSON *args = cJSON_GetObjectItemCaseSensitive(func, "arguments");
                printf("[Tool Call] Running tool '%s' with args: %s\n",
                       cJSON_IsString(name) ? name->valuestring : "unknown",
                       cJSON_IsString(args) ? args->valuestring : "{}");
                fflush(stdout);
            }

            status = handle_tool_calls(messages, tool_calls);
            cJSON_Delete(response);
            if (fallback_tool_calls) {
                cJSON_Delete(fallback_tool_calls);
            }

            if (status != 0) {
                fprintf(stderr, "error: tool execution failed\n");
                break;
            }

            continue;
        }

        cJSON *assistant_msg_copy = cJSON_Duplicate(message, 1);
        cJSON_AddItemToArray(messages, assistant_msg_copy);
        cJSON_Delete(response);
        status = 0;
        break;
    }

    cJSON_Delete(request);
    return status;
}

int app_run(const agent_config_t *config) {
    if (!config) {
        return 1;
    }

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        fprintf(stderr, "failed to initialize curl globally\n");
        return 1;
    }

    memory_init(config);


    cJSON *messages = cJSON_CreateArray();
    if (!messages) {
        curl_global_cleanup();
        return 1;
    }
    const char *default_system_prompt =
        "You are Claude Code C, a command line AI programming assistant.\n"
        "You have access to a set of tools to interact with the system:\n"
        "  - Read: Read the contents of a file.\n"
        "  - Write: Write or overwrite the content of a file.\n"
        "  - Bash: Run shell commands. Note: Prefer specific files/directory tools for speed and safety.\n"
        "  - ListDirectory: List directories and files in a path.\n"
        "  - FindFiles: Find files matching a name pattern.\n"
        "  - FileSearch: Search for text inside files (grep).\n"
        "  - SysInfo: View system details (OS, user, current directory, shell).\n"
        "  - CreateFile: Create a new empty file at a specified path.\n"
        "  - CreateDirectory: Create a new directory (and intermediate folders) at a specified path.\n\n"
        "Rules:\n"
        "1. Use appropriate tools to gather information before making modifications.\n"
        "2. Ensure you check compile/test status when modifying code.\n"
        "3. Be concise and direct in your explanations.\n"
        "4. Work in the context of the workspace.\n"
        "5. ONLY call tools when strictly necessary to perform file/system operations. For general questions, conversational responses, code explanations, or non-file related queries, do NOT call any tools. Respond directly in plain text.\n"
        "6. When calling tools, ensure that all string arguments (especially file contents containing source code) are strictly JSON-compliant. Specifically, all double quotes must be escaped as \\\", and all newline characters must be escaped as \\n. NEVER include literal raw newlines inside a JSON string value.";

    char *combined_prompt = NULL;
    size_t base_len = strlen(default_system_prompt) + (config->system_prompt ? strlen(config->system_prompt) : 0) + 300;
    combined_prompt = malloc(base_len);
    if (combined_prompt) {
        if (config->system_prompt && strlen(config->system_prompt) > 0) {
            snprintf(combined_prompt, base_len, "%s\n\nUser Custom System Prompt:\n%s",
                     default_system_prompt, config->system_prompt);
        } else {
            snprintf(combined_prompt, base_len, "%s", default_system_prompt);
        }

        if (config->provider == PROVIDER_GROQ || config->provider == PROVIDER_OLLAMA) {
            strcat(combined_prompt,
                "\n\nIMPORTANT: You must invoke tools ONLY using the native tool-calling mechanism. "
                "Do NOT output XML-like tags (such as <function=...> or <tools>...</tools>) or text descriptions of the tool calls. "
                "Simply call the tool using the API.");
        }
    }

    cJSON *system_msg = cJSON_CreateObject();
    if (system_msg) {
        cJSON_AddStringToObject(system_msg, "role", "system");
        cJSON_AddStringToObject(system_msg, "content", combined_prompt ? combined_prompt : default_system_prompt);
        cJSON_AddItemToArray(messages, system_msg);
    }
    free(combined_prompt);

    int status = 0;

    if (!config->interactive) {
        if (!config->prompt || strlen(config->prompt) == 0) {
            fprintf(stderr, "error: prompt is required in non-interactive mode\n");
            cJSON_Delete(messages);
            curl_global_cleanup();
            return 1;
        }

        cJSON *user_msg = cJSON_CreateObject();
        if (user_msg) {
            cJSON_AddStringToObject(user_msg, "role", "user");
            cJSON_AddStringToObject(user_msg, "content", config->prompt);
            cJSON_AddItemToArray(messages, user_msg);
        }

        memory_retrieve_and_inject(config, config->prompt, messages);
        status = run_agent_loop(config, messages);
        memory_auto_extract(config, messages);
    } else {
        const char *provider_name = "Unknown";
        switch (config->provider) {
            case PROVIDER_OPENROUTER: provider_name = "OpenRouter"; break;
            case PROVIDER_OPENAI: provider_name = "OpenAI"; break;
            case PROVIDER_ANTHROPIC: provider_name = "Anthropic"; break;
            case PROVIDER_GEMINI: provider_name = "Gemini"; break;
            case PROVIDER_GROQ: provider_name = "Groq"; break;
            case PROVIDER_OLLAMA: provider_name = "Ollama"; break;
        }

        printf("%s (AI-agnostic coding assistant)\n", APP_NAME);
        printf("Provider: %s | Model: %s\n", provider_name, config->model);
        printf("Type 'exit' or 'quit' to end session.\n");
        printf("Shortcuts: '/settings' or '/provider' (setup), '/model' (change model), '/key' (change key), '/info' (current config).\n");
        printf("New Features: '/memory' (view memories), '/memory add <text>', '/speculate' (speculative execution sandbox).\n\n");
        fflush(stdout);

        char input[8192];
        while (1) {
            printf("%s > ", APP_NAME);
            fflush(stdout);

            if (!fgets(input, sizeof(input), stdin)) {
                break;
            }

            size_t len = strlen(input);
            if (len > 0 && input[len - 1] == '\n') {
                input[len - 1] = '\0';
                len--;
            }

            if (strcmp(input, "exit") == 0 || strcmp(input, "quit") == 0) {
                break;
            }

            if (strcmp(input, "/settings") == 0 || strcmp(input, "/provider") == 0) {
                agent_config_t *mutable_config = (agent_config_t *)config;
                if (config_interactive_setup(mutable_config) == 0) {
                    const char *new_prov_name = "Unknown";
                    switch (mutable_config->provider) {
                        case PROVIDER_OPENROUTER: new_prov_name = "OpenRouter"; break;
                        case PROVIDER_OPENAI: new_prov_name = "OpenAI"; break;
                        case PROVIDER_ANTHROPIC: new_prov_name = "Anthropic"; break;
                        case PROVIDER_GEMINI: new_prov_name = "Gemini"; break;
                        case PROVIDER_GROQ: new_prov_name = "Groq"; break;
                        case PROVIDER_OLLAMA: new_prov_name = "Ollama"; break;
                    }
                    printf("\033[1;32m✔ Configuration updated!\033[0m Provider: %s | Model: %s\n\n", new_prov_name, mutable_config->model);
                }
                continue;
            }
            if (strcmp(input, "/model") == 0) {
                agent_config_t *mutable_config = (agent_config_t *)config;
                if (config_select_model_interactive(mutable_config) == 0) {
                    printf("\033[1;32m✔ Model updated!\033[0m Model: %s\n\n", mutable_config->model);
                }
                continue;
            }

            if (strcmp(input, "/key") == 0) {
                agent_config_t *mutable_config = (agent_config_t *)config;
                if (mutable_config->provider == PROVIDER_OLLAMA) {
                    printf("Ollama does not require an API key.\n\n");
                    continue;
                }
                const char *prov_name = "Unknown";
                switch (mutable_config->provider) {
                    case PROVIDER_OPENROUTER: prov_name = "OpenRouter"; break;
                    case PROVIDER_OPENAI: prov_name = "OpenAI"; break;
                    case PROVIDER_ANTHROPIC: prov_name = "Anthropic"; break;
                    case PROVIDER_GEMINI: prov_name = "Gemini"; break;
                    case PROVIDER_GROQ: prov_name = "Groq"; break;
                    case PROVIDER_OLLAMA: prov_name = "Ollama"; break;
                }
                char prompt[256];
                snprintf(prompt, sizeof(prompt), "Enter API Key for %s", prov_name);
                char *entered = prompt_text_input(prompt, 1);
                if (entered) {
                    if (mutable_config->api_key) free(mutable_config->api_key);
                    mutable_config->api_key = entered;
                    config_save(mutable_config);
                    printf("\033[1;32m✔ API Key updated and saved!\033[0m\n\n");
                }
                continue;
            }

            if (strcmp(input, "/info") == 0 || strcmp(input, "/config") == 0) {
                const char *prov_name = "Unknown";
                switch (config->provider) {
                    case PROVIDER_OPENROUTER: prov_name = "OpenRouter"; break;
                    case PROVIDER_OPENAI: prov_name = "OpenAI"; break;
                    case PROVIDER_ANTHROPIC: prov_name = "Anthropic"; break;
                    case PROVIDER_GEMINI: prov_name = "Gemini"; break;
                    case PROVIDER_GROQ: prov_name = "Groq"; break;
                    case PROVIDER_OLLAMA: prov_name = "Ollama"; break;
                }
                printf("Current Configuration:\n");
                printf("  Provider: %s\n", prov_name);
                printf("  Model:    %s\n", config->model ? config->model : "(none)");
                printf("  Base URL: %s\n", config->base_url ? config->base_url : "(default)");
                printf("  API Key:  %s\n\n", (config->api_key && strlen(config->api_key) > 0) ? "********" : "(not set)");
                continue;
            }

            if (strcmp(input, "/memory") == 0 || strcmp(input, "/memories") == 0) {
                memory_print_all();
                continue;
            }
            if (strncmp(input, "/memory add ", 12) == 0) {
                memory_add(config, input + 12, "manual", 8);
                printf("\033[1;32m✔ Memory added successfully!\033[0m\n\n");
                continue;
            }
            if (strcmp(input, "/speculate") == 0) {
                char count_buf[32];
                printf("Enter number of branches to run (2-3): ");
                fflush(stdout);
                if (fgets(count_buf, sizeof(count_buf), stdin)) {
                    int num_branches = atoi(count_buf);
                    if (num_branches < 2 || num_branches > 3) {
                        printf("Invalid number of branches. Must be 2 or 3.\n\n");
                        continue;
                    }
                    char test_cmd[256] = {0};
                    printf("Enter test command (optional, e.g. 'cmake --build build'): ");
                    fflush(stdout);
                    if (fgets(test_cmd, sizeof(test_cmd), stdin)) {
                        size_t cmd_len = strlen(test_cmd);
                        if (cmd_len > 0 && test_cmd[cmd_len - 1] == '\n') {
                            test_cmd[cmd_len - 1] = '\0';
                        }
                    }

                    const char *prompts[3];
                    char prompt_buffers[3][2048];
                    int prompt_ok = 1;
                    for (int i = 0; i < num_branches; i++) {
                        printf("Enter Prompt for Branch %d: ", i + 1);
                        fflush(stdout);
                        if (fgets(prompt_buffers[i], sizeof(prompt_buffers[i]), stdin)) {
                            size_t p_len = strlen(prompt_buffers[i]);
                            if (p_len > 0 && prompt_buffers[i][p_len - 1] == '\n') {
                                prompt_buffers[i][p_len - 1] = '\0';
                            }
                            prompts[i] = prompt_buffers[i];
                        } else {
                            prompt_ok = 0;
                            break;
                        }
                    }

                    if (prompt_ok) {
                        speculate_run(config, num_branches, prompts, test_cmd);
                    }
                }
                continue;
            }

            if (len == 0) {
                continue;
            }

            cJSON *user_msg = cJSON_CreateObject();
            if (user_msg) {
                cJSON_AddStringToObject(user_msg, "role", "user");
                cJSON_AddStringToObject(user_msg, "content", input);
                cJSON_AddItemToArray(messages, user_msg);
            }

            memory_retrieve_and_inject(config, input, messages);
            status = run_agent_loop(config, messages);
            memory_auto_extract(config, messages);
            printf("\n");
            fflush(stdout);
        }
    }

    cJSON_Delete(messages);
    curl_global_cleanup();
    return status;
}
