#ifndef MEMORY_H
#define MEMORY_H

#include "config.h"
#include <cjson/cJSON.h>

// Initialize/load semantic memories for the project
int memory_init(const agent_config_t *config);

// Add a new memory with optional metadata
int memory_add(const agent_config_t *config, const char *text, const char *domain, int importance);

// Query memories similar to query_text and inject them into the system prompt or messages
int memory_retrieve_and_inject(const agent_config_t *config, const char *query_text, cJSON *messages);

// Auto-extract memories from the conversation history
int memory_auto_extract(const agent_config_t *config, const cJSON *messages);

// Print all memories (for the /memory REPL command)
void memory_print_all(void);

#endif
