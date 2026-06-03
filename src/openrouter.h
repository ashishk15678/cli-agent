#ifndef OPENROUTER_H
#define OPENROUTER_H

#include <cjson/cJSON.h>

int openrouter_chat_completion(const char *base_url, const char *api_key, const cJSON *request, cJSON **response_out);

#endif
