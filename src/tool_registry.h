#ifndef TOOL_REGISTRY_H
#define TOOL_REGISTRY_H

#include <cjson/cJSON.h>

cJSON *tool_registry_build_schema(void);
int tool_registry_execute(const cJSON *tool_call, char **output_out);

#endif
