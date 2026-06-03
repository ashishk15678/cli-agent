#include "config.h"
#include "utils.h"
#include "dropdown.h"

#include <cjson/cJSON.h>
#include <getopt.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void print_help(const char *prog_name) {
    printf("Usage: %s [options]\n\n", prog_name);
    printf("Options:\n");
    printf("  -p, --prompt <string>         Run a single prompt non-interactively and exit\n");
    printf("  -i, --interactive             Start an interactive REPL session (default)\n");
    printf("  -c, --choose-model            Select provider and model interactively at startup\n");
    printf("  -r, --provider <provider>     AI provider: openrouter, openai, anthropic, gemini, groq, ollama\n");
    printf("  -m, --model <model>           Model name to use\n");
    printf("  -k, --api-key <key>           API key for the provider\n");
    printf("  -u, --base-url <url>          Custom base URL endpoint\n");
    printf("  -s, --system-prompt <file>    Path to a file containing custom system prompt\n");
    printf("  --user-system-prompt <str>    Custom system prompt string directly\n");
    printf("  -h, --help                    Show this help message\n\n");
    printf("Environment Variables:\n");
    printf("  AI_PROVIDER, AI_MODEL, AI_API_KEY, AI_BASE_URL, AI_SYSTEM_PROMPT_FILE\n");
}

static char *get_config_path(void) {
    const char *home = getenv("HOME");
    if (!home) return NULL;
    size_t len = strlen(home) + 32;
    char *path = malloc(len);
    if (path) {
        snprintf(path, len, "%s/.claude-code-c.json", home);
    }
    return path;
}

static const char *provider_to_str(ai_provider_t provider) {
    switch (provider) {
        case PROVIDER_OPENROUTER: return "openrouter";
        case PROVIDER_OPENAI: return "openai";
        case PROVIDER_ANTHROPIC: return "anthropic";
        case PROVIDER_GEMINI: return "gemini";
        case PROVIDER_GROQ: return "groq";
        case PROVIDER_OLLAMA: return "ollama";
        default: return "openrouter";
    }
}

static ai_provider_t str_to_provider(const char *str) {
    if (!str) return PROVIDER_OPENROUTER;
    if (strcasecmp(str, "openai") == 0) return PROVIDER_OPENAI;
    if (strcasecmp(str, "anthropic") == 0) return PROVIDER_ANTHROPIC;
    if (strcasecmp(str, "gemini") == 0) return PROVIDER_GEMINI;
    if (strcasecmp(str, "groq") == 0) return PROVIDER_GROQ;
    if (strcasecmp(str, "ollama") == 0) return PROVIDER_OLLAMA;
    return PROVIDER_OPENROUTER;
}

static const char *get_default_base_url(ai_provider_t provider) {
    switch (provider) {
        case PROVIDER_OPENROUTER: return "https://openrouter.ai/api/v1";
        case PROVIDER_OPENAI: return "https://api.openai.com/v1";
        case PROVIDER_ANTHROPIC: return "https://api.anthropic.com/v1";
        case PROVIDER_GEMINI: return "https://generativelanguage.googleapis.com/v1beta/openai";
        case PROVIDER_GROQ: return "https://api.groq.com/openai/v1";
        case PROVIDER_OLLAMA: return "http://localhost:11434/v1";
        default: return "";
    }
}

static int is_default_base_url_of_any(const char *url) {
    if (!url) return 0;
    for (int i = 0; i <= PROVIDER_OLLAMA; i++) {
        if (strcmp(url, get_default_base_url((ai_provider_t)i)) == 0) {
            return 1;
        }
    }
    return 0;
}

static void cJSON_replace_or_add_string(cJSON *object, const char *key, const char *value) {
    if (!object || !key) return;
    cJSON *existing = cJSON_GetObjectItemCaseSensitive(object, key);
    if (existing) {
        cJSON_SetValuestring(existing, value ? value : "");
    } else {
        cJSON_AddStringToObject(object, key, value ? value : "");
    }
}

void config_load_for_provider(agent_config_t *config, ai_provider_t provider) {
    char *path = get_config_path();
    if (!path) return;

    if (access(path, F_OK) != 0) {
        free(path);
        return;
    }

    size_t size = 0;
    char *content = read_entire_file(path, &size);
    if (!content) {
        free(path);
        return;
    }

    cJSON *root = cJSON_Parse(content);
    free(content);
    if (!root) {
        free(path);
        return;
    }

    const char *prov_str = provider_to_str(provider);

    cJSON *api_keys = cJSON_GetObjectItemCaseSensitive(root, "api_keys");
    if (cJSON_IsObject(api_keys)) {
        cJSON *key_item = cJSON_GetObjectItemCaseSensitive(api_keys, prov_str);
        if (cJSON_IsString(key_item) && key_item->valuestring && strlen(key_item->valuestring) > 0) {
            if (config->api_key) free(config->api_key);
            config->api_key = duplicate_string(key_item->valuestring);
        } else {
            if (config->api_key) {
                free(config->api_key);
                config->api_key = NULL;
            }
        }
    } else {
        if (config->api_key) {
            free(config->api_key);
            config->api_key = NULL;
        }
    }

    cJSON *models = cJSON_GetObjectItemCaseSensitive(root, "models");
    if (cJSON_IsObject(models)) {
        cJSON *model_item = cJSON_GetObjectItemCaseSensitive(models, prov_str);
        if (cJSON_IsString(model_item) && model_item->valuestring && strlen(model_item->valuestring) > 0) {
            if (config->model) free(config->model);
            config->model = duplicate_string(model_item->valuestring);
        } else {
            if (config->model) {
                free(config->model);
                config->model = NULL;
            }
        }
    } else {
        if (config->model) {
            free(config->model);
            config->model = NULL;
        }
    }

    cJSON *base_urls = cJSON_GetObjectItemCaseSensitive(root, "base_urls");
    if (cJSON_IsObject(base_urls)) {
        cJSON *url_item = cJSON_GetObjectItemCaseSensitive(base_urls, prov_str);
        if (cJSON_IsString(url_item) && url_item->valuestring && strlen(url_item->valuestring) > 0) {
            if (config->base_url) free(config->base_url);
            config->base_url = duplicate_string(url_item->valuestring);
        } else {
            if (config->base_url) {
                free(config->base_url);
                config->base_url = NULL;
            }
        }
    } else {
        if (config->base_url) {
            free(config->base_url);
            config->base_url = NULL;
        }
    }

    // Self-healing: if base_url is a default of another provider, reset it to this provider's default
    if (config->base_url && is_default_base_url_of_any(config->base_url)) {
        const char *expected = get_default_base_url(provider);
        if (strcmp(config->base_url, expected) != 0) {
            free(config->base_url);
            config->base_url = duplicate_string(expected);
            config_save(config);
        }
    }

    cJSON_Delete(root);
    free(path);
}

void config_load(agent_config_t *config) {
    char *path = get_config_path();
    if (!path) return;

    if (access(path, F_OK) != 0) {
        free(path);
        return;
    }

    size_t size = 0;
    char *content = read_entire_file(path, &size);
    if (!content) {
        free(path);
        return;
    }

    cJSON *root = cJSON_Parse(content);
    free(content);
    if (!root) {
        free(path);
        return;
    }

    cJSON *prov_item = cJSON_GetObjectItemCaseSensitive(root, "provider");
    if (cJSON_IsString(prov_item)) {
        config->provider = str_to_provider(prov_item->valuestring);
    }

    const char *prov_str = provider_to_str(config->provider);

    cJSON *api_keys = cJSON_GetObjectItemCaseSensitive(root, "api_keys");
    if (cJSON_IsObject(api_keys)) {
        cJSON *key_item = cJSON_GetObjectItemCaseSensitive(api_keys, prov_str);
        if (cJSON_IsString(key_item) && key_item->valuestring && strlen(key_item->valuestring) > 0) {
            if (config->api_key) free(config->api_key);
            config->api_key = duplicate_string(key_item->valuestring);
        }
    }

    cJSON *models = cJSON_GetObjectItemCaseSensitive(root, "models");
    if (cJSON_IsObject(models)) {
        cJSON *model_item = cJSON_GetObjectItemCaseSensitive(models, prov_str);
        if (cJSON_IsString(model_item) && model_item->valuestring && strlen(model_item->valuestring) > 0) {
            if (config->model) free(config->model);
            config->model = duplicate_string(model_item->valuestring);
        }
    }

    cJSON *base_urls = cJSON_GetObjectItemCaseSensitive(root, "base_urls");
    if (cJSON_IsObject(base_urls)) {
        cJSON *url_item = cJSON_GetObjectItemCaseSensitive(base_urls, prov_str);
        if (cJSON_IsString(url_item) && url_item->valuestring && strlen(url_item->valuestring) > 0) {
            if (config->base_url) free(config->base_url);
            config->base_url = duplicate_string(url_item->valuestring);
        }
    }

    // Self-healing: if base_url is a default of another provider, reset it to this provider's default
    if (config->base_url && is_default_base_url_of_any(config->base_url)) {
        const char *expected = get_default_base_url(config->provider);
        if (strcmp(config->base_url, expected) != 0) {
            free(config->base_url);
            config->base_url = duplicate_string(expected);
            config_save(config);
        }
    }

    cJSON_Delete(root);
    free(path);
}

int config_save(const agent_config_t *config) {
    char *path = get_config_path();
    if (!path) return 1;

    cJSON *root = NULL;
    size_t size = 0;
    char *content = read_entire_file(path, &size);
    if (content) {
        root = cJSON_Parse(content);
        free(content);
    }

    if (!root) {
        root = cJSON_CreateObject();
    }

    if (!root) {
        free(path);
        return 1;
    }

    // Save active provider
    cJSON_replace_or_add_string(root, "provider", provider_to_str(config->provider));

    // Save/update keys, models, base_urls objects
    cJSON *api_keys = cJSON_GetObjectItemCaseSensitive(root, "api_keys");
    if (!api_keys) {
        api_keys = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "api_keys", api_keys);
    }
    if (config->api_key) {
        cJSON_replace_or_add_string(api_keys, provider_to_str(config->provider), config->api_key);
    }

    cJSON *models = cJSON_GetObjectItemCaseSensitive(root, "models");
    if (!models) {
        models = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "models", models);
    }
    if (config->model) {
        cJSON_replace_or_add_string(models, provider_to_str(config->provider), config->model);
    }

    cJSON *base_urls = cJSON_GetObjectItemCaseSensitive(root, "base_urls");
    if (!base_urls) {
        base_urls = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "base_urls", base_urls);
    }
    if (config->base_url) {
        cJSON_replace_or_add_string(base_urls, provider_to_str(config->provider), config->base_url);
    }

    char *rendered = cJSON_Print(root);
    cJSON_Delete(root);

    if (rendered) {
        int ret = write_entire_file(path, rendered);
        free(rendered);
        free(path);
        return ret;
    }

    free(path);
    return 1;
}

void config_init(agent_config_t *config) {
    config->provider = PROVIDER_OPENROUTER;
    config->model = NULL;
    config->api_key = NULL;
    config->base_url = NULL;
    config->system_prompt = NULL;
    config->prompt = NULL;
    config->interactive = 1;

    // Load from persistent config file
    config_load(config);

    // Load from environment variables if present
    const char *env_provider = getenv("AI_PROVIDER");
    if (env_provider) {
        ai_provider_t prev = config->provider;
        if (strcasecmp(env_provider, "openai") == 0) config->provider = PROVIDER_OPENAI;
        else if (strcasecmp(env_provider, "anthropic") == 0) config->provider = PROVIDER_ANTHROPIC;
        else if (strcasecmp(env_provider, "gemini") == 0) config->provider = PROVIDER_GEMINI;
        else if (strcasecmp(env_provider, "groq") == 0) config->provider = PROVIDER_GROQ;
        else if (strcasecmp(env_provider, "ollama") == 0) config->provider = PROVIDER_OLLAMA;
        else if (strcasecmp(env_provider, "openrouter") == 0) config->provider = PROVIDER_OPENROUTER;
        
        if (config->provider != prev) {
            config_load_for_provider(config, config->provider);
        }
    }

    const char *env_model = getenv("AI_MODEL");
    if (env_model) {
        if (config->model) free(config->model);
        config->model = duplicate_string(env_model);
    }

    const char *env_api_key = getenv("AI_API_KEY");
    if (env_api_key) {
        if (config->api_key) free(config->api_key);
        config->api_key = duplicate_string(env_api_key);
    } else {
        // Provider-specific fallback
        const char *specific_key = NULL;
        switch (config->provider) {
            case PROVIDER_OPENAI: specific_key = getenv("OPENAI_API_KEY"); break;
            case PROVIDER_ANTHROPIC: specific_key = getenv("ANTHROPIC_API_KEY"); break;
            case PROVIDER_GEMINI: specific_key = getenv("GEMINI_API_KEY"); break;
            case PROVIDER_GROQ: specific_key = getenv("GROQ_API_KEY"); break;
            case PROVIDER_OLLAMA: specific_key = NULL; break;
            case PROVIDER_OPENROUTER: specific_key = getenv("OPENROUTER_API_KEY"); break;
        }
        if (specific_key) {
            if (config->api_key) free(config->api_key);
            config->api_key = duplicate_string(specific_key);
        }
    }

    const char *env_base_url = getenv("AI_BASE_URL");
    if (env_base_url) {
        if (config->base_url) free(config->base_url);
        config->base_url = duplicate_string(env_base_url);
    }

    const char *env_sys_prompt_file = getenv("AI_SYSTEM_PROMPT_FILE");
    if (env_sys_prompt_file) {
        size_t size = 0;
        char *content = read_entire_file(env_sys_prompt_file, &size);
        if (content) config->system_prompt = content;
    }
}

void config_free(agent_config_t *config) {
    if (config->model) free(config->model);
    if (config->api_key) free(config->api_key);
    if (config->base_url) free(config->base_url);
    if (config->system_prompt) free(config->system_prompt);
    if (config->prompt) free(config->prompt);
}

int config_parse_args(agent_config_t *config, int argc, char *argv[]) {
    static struct option long_options[] = {
        {"prompt", required_argument, 0, 'p'},
        {"interactive", no_argument, 0, 'i'},
        {"choose-model", no_argument, 0, 'c'},
        {"provider", required_argument, 0, 'r'},
        {"model", required_argument, 0, 'm'},
        {"api-key", required_argument, 0, 'k'},
        {"base-url", required_argument, 0, 'u'},
        {"system-prompt", required_argument, 0, 's'},
        {"user-system-prompt", required_argument, 0, 'x'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    int option_index = 0;

    // Reset getopt
    optind = 1;

    while ((opt = getopt_long(argc, argv, "p:icr:m:k:u:s:h", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'p':
                if (config->prompt) free(config->prompt);
                config->prompt = duplicate_string(optarg);
                config->interactive = 0;
                break;
            case 'i':
                config->interactive = 1;
                break;
            case 'c':
                if (config_interactive_setup(config) != 0) {
                    fprintf(stderr, "Interactive configuration canceled.\n");
                    return 1;
                }
                break;
            case 'r':
                if (strcasecmp(optarg, "openai") == 0) config->provider = PROVIDER_OPENAI;
                else if (strcasecmp(optarg, "anthropic") == 0) config->provider = PROVIDER_ANTHROPIC;
                else if (strcasecmp(optarg, "gemini") == 0) config->provider = PROVIDER_GEMINI;
                else if (strcasecmp(optarg, "groq") == 0) config->provider = PROVIDER_GROQ;
                else if (strcasecmp(optarg, "ollama") == 0) config->provider = PROVIDER_OLLAMA;
                else if (strcasecmp(optarg, "openrouter") == 0) config->provider = PROVIDER_OPENROUTER;
                else {
                    fprintf(stderr, "Unknown provider: %s\n", optarg);
                    return 1;
                }
                break;
            case 'm':
                if (config->model) free(config->model);
                config->model = duplicate_string(optarg);
                break;
            case 'k':
                if (config->api_key) free(config->api_key);
                config->api_key = duplicate_string(optarg);
                break;
            case 'u':
                if (config->base_url) free(config->base_url);
                config->base_url = duplicate_string(optarg);
                break;
            case 's': {
                size_t size = 0;
                char *content = read_entire_file(optarg, &size);
                if (!content) {
                    fprintf(stderr, "Failed to read system prompt file: %s\n", optarg);
                    return 1;
                }
                if (config->system_prompt) free(config->system_prompt);
                config->system_prompt = content;
                break;
            }
            case 'x':
                if (config->system_prompt) free(config->system_prompt);
                config->system_prompt = duplicate_string(optarg);
                break;
            case 'h':
                print_help(argv[0]);
                exit(0);
            default:
                print_help(argv[0]);
                return 1;
        }
    }

    // Assign defaults after parsing args if still NULL
    if (!config->base_url) {
        switch (config->provider) {
            case PROVIDER_OPENROUTER: config->base_url = duplicate_string("https://openrouter.ai/api/v1"); break;
            case PROVIDER_OPENAI: config->base_url = duplicate_string("https://api.openai.com/v1"); break;
            case PROVIDER_ANTHROPIC: config->base_url = duplicate_string("https://api.anthropic.com/v1"); break;
            case PROVIDER_GEMINI: config->base_url = duplicate_string("https://generativelanguage.googleapis.com/v1beta/openai"); break;
            case PROVIDER_GROQ: config->base_url = duplicate_string("https://api.groq.com/openai/v1"); break;
            case PROVIDER_OLLAMA: config->base_url = duplicate_string("http://localhost:11434/v1"); break;
        }
    }

    if (!config->model) {
        switch (config->provider) {
            case PROVIDER_OPENROUTER: config->model = duplicate_string("anthropic/claude-3.5-sonnet:beta"); break;
            case PROVIDER_OPENAI: config->model = duplicate_string("gpt-4o"); break;
            case PROVIDER_ANTHROPIC: config->model = duplicate_string("claude-3-5-sonnet-20241022"); break;
            case PROVIDER_GEMINI: config->model = duplicate_string("gemini-1.5-pro"); break;
            case PROVIDER_GROQ: config->model = duplicate_string("llama-3.3-70b-versatile"); break;
            case PROVIDER_OLLAMA: config->model = duplicate_string("qwen2.5-coder:7b"); break;
        }
    }

    return 0;
}

static const char *provider_names[] = {
    "OpenRouter",
    "OpenAI",
    "Anthropic",
    "Gemini",
    "Groq",
    "Ollama"
};

static const ai_provider_t provider_vals[] = {
    PROVIDER_OPENROUTER,
    PROVIDER_OPENAI,
    PROVIDER_ANTHROPIC,
    PROVIDER_GEMINI,
    PROVIDER_GROQ,
    PROVIDER_OLLAMA
};

#define NUM_PROVIDERS 6

static const char *openrouter_models[] = {
    "anthropic/claude-3.5-sonnet:beta",
    "anthropic/claude-3-opus",
    "google/gemini-2.5-pro",
    "google/gemini-2.5-flash",
    "meta-llama/llama-3.3-70b-instruct",
    "deepseek/deepseek-chat"
};

static const char *openai_models[] = {
    "gpt-4o",
    "gpt-4o-mini",
    "o1-preview",
    "o1-mini"
};

static const char *anthropic_models[] = {
    "claude-3-5-sonnet-20241022",
    "claude-3-5-haiku-20241022",
    "claude-3-opus-20240229"
};

static const char *gemini_models[] = {
    "gemini-2.5-flash",
    "gemini-2.5-pro",
    "gemini-1.5-flash",
    "gemini-1.5-pro"
};

static const char *groq_models[] = {
    "llama-3.3-70b-versatile",
    "llama-3.1-8b-instant",
    "mixtral-8x7b-32768",
    "gemma2-9b-it"
};

static const char *ollama_models[] = {
    "qwen2.5-coder:7b",
    "llama3.1",
    "codellama",
    "mistral"
};
int config_interactive_setup(agent_config_t *config) {
    int prov_idx = 0;
    for (int i = 0; i < NUM_PROVIDERS; i++) {
        if (config->provider == provider_vals[i]) {
            prov_idx = i;
            break;
        }
    }

    int chosen_prov = dropdown_select("Select AI Provider", provider_names, NUM_PROVIDERS, prov_idx);
    if (chosen_prov == -1) return -1;

    config->provider = provider_vals[chosen_prov];

    // Load saved settings for this provider if any
    config_load_for_provider(config, config->provider);

    int chosen_model = config_select_model_interactive(config);
    if (chosen_model == -1) return -1;

    if (!config->base_url) {
        switch (config->provider) {
            case PROVIDER_OPENROUTER: config->base_url = duplicate_string("https://openrouter.ai/api/v1"); break;
            case PROVIDER_OPENAI: config->base_url = duplicate_string("https://api.openai.com/v1"); break;
            case PROVIDER_ANTHROPIC: config->base_url = duplicate_string("https://api.anthropic.com/v1"); break;
            case PROVIDER_GEMINI: config->base_url = duplicate_string("https://generativelanguage.googleapis.com/v1beta/openai"); break;
            case PROVIDER_GROQ: config->base_url = duplicate_string("https://api.groq.com/openai/v1"); break;
            case PROVIDER_OLLAMA: config->base_url = duplicate_string("http://localhost:11434/v1"); break;
        }
    }

    if (config->provider != PROVIDER_OLLAMA) {
        const char *env_key = NULL;
        const char *key_name = "";
        switch (config->provider) {
            case PROVIDER_OPENAI: env_key = getenv("OPENAI_API_KEY"); key_name = "OPENAI_API_KEY"; break;
            case PROVIDER_ANTHROPIC: env_key = getenv("ANTHROPIC_API_KEY"); key_name = "ANTHROPIC_API_KEY"; break;
            case PROVIDER_GEMINI: env_key = getenv("GEMINI_API_KEY"); key_name = "GEMINI_API_KEY"; break;
            case PROVIDER_GROQ: env_key = getenv("GROQ_API_KEY"); key_name = "GROQ_API_KEY"; break;
            case PROVIDER_OPENROUTER: env_key = getenv("OPENROUTER_API_KEY"); key_name = "OPENROUTER_API_KEY"; break;
            default: break;
        }

        if (!config->api_key || !*config->api_key) {
            if (env_key && *env_key) {
                config->api_key = duplicate_string(env_key);
                printf("\033[1;32m✔\033[0m Found API key in environment variable '%s'\n", key_name);
            } else {
                char prompt[256];
                snprintf(prompt, sizeof(prompt), "Enter API Key for %s", provider_names[chosen_prov]);
                char *entered = prompt_text_input(prompt, 1);
                if (entered) {
                    config->api_key = entered;
                    setenv(key_name, entered, 1);
                    setenv("AI_API_KEY", entered, 1);
                } else {
                    return -1;
                }
            }
        }
    }

    config_save(config);
    return 0;
}

int config_select_model_interactive(agent_config_t *config) {
    const char **models = NULL;
    int num_models = 0;
    int default_idx = 0;

    switch (config->provider) {
        case PROVIDER_OPENROUTER:
            models = openrouter_models;
            num_models = sizeof(openrouter_models) / sizeof(openrouter_models[0]);
            break;
        case PROVIDER_OPENAI:
            models = openai_models;
            num_models = sizeof(openai_models) / sizeof(openai_models[0]);
            break;
        case PROVIDER_ANTHROPIC:
            models = anthropic_models;
            num_models = sizeof(anthropic_models) / sizeof(anthropic_models[0]);
            break;
        case PROVIDER_GEMINI:
            models = gemini_models;
            num_models = sizeof(gemini_models) / sizeof(gemini_models[0]);
            break;
        case PROVIDER_GROQ:
            models = groq_models;
            num_models = sizeof(groq_models) / sizeof(groq_models[0]);
            break;
        case PROVIDER_OLLAMA:
            models = ollama_models;
            num_models = sizeof(ollama_models) / sizeof(ollama_models[0]);
            break;
    }

    if (config->model && num_models > 0) {
        for (int i = 0; i < num_models; i++) {
            if (strcmp(config->model, models[i]) == 0) {
                default_idx = i;
                break;
            }
        }
    }

    int idx = dropdown_select("Select Model", models, num_models, default_idx);
    if (idx == -1) return -1;

    if (config->model) free(config->model);
    config->model = duplicate_string(models[idx]);
    config_save(config);
    return 0;
}
