#ifndef PROVIDER_H
#define PROVIDER_H

#include "config.h"
#include <cjson/cJSON.h>

int provider_chat_completion(const agent_config_t *config, const cJSON *request, cJSON **response_out);

cJSON *translate_to_anthropic(const cJSON *openai_req);
cJSON *translate_from_anthropic(const cJSON *anthropic_resp);

#endif
