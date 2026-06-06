#ifndef DROPDOWN_H
#define DROPDOWN_H

/**
 * Renders an interactive CLI dropdown selection menu.
 * Returns the 0-based index of the chosen option, or -1 if canceled (Esc).
 */
int dropdown_select(const char *title, const char *options[], int num_options, int default_index);

/**
 * Helper to prompt the user for text input securely (e.g. for API keys).
 * Returns a dynamically allocated string, or NULL if canceled/empty.
 */
char *prompt_text_input(const char *prompt, int is_password);

#endif
