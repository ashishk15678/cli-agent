#ifndef SPECULATE_H
#define SPECULATE_H

#include "config.h"

// Run speculative parallel execution
// Takes the original config, parallel implementation prompts to try, and the command to run tests.
// Scores their performance and applies the winning changes back to the workspace.
int speculate_run(const agent_config_t *config, int num_branches, const char **branch_prompts, const char *test_command);

#endif
