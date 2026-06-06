#ifndef DRIFT_H
#define DRIFT_H

#include "config.h"

// Initialize goal drift tracking with the original prompt
void drift_init(const char *original_prompt);

// Check if the current modifications have drifted from the original goal.
// Returns 1 if drift detected, 0 otherwise. If drifted, explanation_out will contain the reason.
int drift_check(const agent_config_t *config, char **explanation_out);

// Reset drift tracking
void drift_reset(void);

// Ask the user what to do when drift is detected (continue, revert, etc.)
// Returns 0 to continue, 1 to revert changes
int drift_handle_prompt(const agent_config_t *config, const char *explanation);

#endif
