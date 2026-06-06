#ifndef CONFIG_H
#define CONFIG_H

#define APP_NAME "sage"

typedef enum {
    PROVIDER_OPENROUTER,
    PROVIDER_OPENAI,
    PROVIDER_ANTHROPIC,
    PROVIDER_GEMINI,
    PROVIDER_GROQ,
    PROVIDER_OLLAMA
} ai_provider_t;

typedef struct {
    ai_provider_t provider;
    char *model;
    char *api_key;
    char *base_url;
    char *system_prompt;
    char *prompt;
    int interactive;
} agent_config_t;

void config_init(agent_config_t *config);
void config_free(agent_config_t *config);
int config_parse_args(agent_config_t *config, int argc, char *argv[]);
void print_help(const char *prog_name);
int config_interactive_setup(agent_config_t *config);
int config_select_model_interactive(agent_config_t *config);
int config_save(const agent_config_t *config);
void config_load(agent_config_t *config);
void config_load_for_provider(agent_config_t *config, ai_provider_t provider);

#endif
