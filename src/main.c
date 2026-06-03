#include "app.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    agent_config_t config;
    config_init(&config);

    if (config_parse_args(&config, argc, argv) != 0) {
        config_free(&config);
        return 1;
    }

    // Ollama does not require an API key by default
    if (config.provider != PROVIDER_OLLAMA) {
        if (!config.api_key || !*config.api_key) {
            if (config.interactive && isatty(STDIN_FILENO)) {
                printf("No configuration detected. Entering interactive setup...\n\n");
                if (config_interactive_setup(&config) != 0) {
                    fprintf(stderr, "Interactive configuration canceled.\n");
                    config_free(&config);
                    return 1;
                }
            } else {
                fprintf(stderr, "error: API key is not set. Please set the appropriate environment variable (e.g. AI_API_KEY, OPENAI_API_KEY, OPENROUTER_API_KEY) or provide it using -k/--api-key option.\n");
                config_free(&config);
                return 1;
            }
        }
    }

    int result = app_run(&config);

    config_free(&config);
    return result;
}
