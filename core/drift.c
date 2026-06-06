#include "drift.h"
#include "provider.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>

static char *saved_original_prompt = NULL;
static char *last_checked_files = NULL;

void drift_init(const char *original_prompt) {
    drift_reset();
    if (original_prompt) {
        saved_original_prompt = strdup(original_prompt);
    }
}

void drift_reset(void) {
    if (saved_original_prompt) {
        free(saved_original_prompt);
        saved_original_prompt = NULL;
    }
    if (last_checked_files) {
        free(last_checked_files);
        last_checked_files = NULL;
    }
}

// Run a shell command and return its stdout as a string
static char *exec_command(const char *command) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return NULL;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return NULL;
    }

    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execl("/bin/bash", "bash", "-c", command, (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);

    struct string_buf buf = {NULL, 0};
    char chunk[4096];
    for (;;) {
        ssize_t read_len = read(pipefd[0], chunk, sizeof(chunk));
        if (read_len < 0) {
            if (errno == EINTR) continue;
            free(buf.data);
            close(pipefd[0]);
            waitpid(pid, NULL, 0);
            return NULL;
        }
        if (read_len == 0) break;

        if (string_buf_append(&buf, chunk, (size_t)read_len) != 0) {
            free(buf.data);
            close(pipefd[0]);
            waitpid(pid, NULL, 0);
            return NULL;
        }
    }

    close(pipefd[0]);
    waitpid(pid, NULL, 0);

    if (!buf.data) {
        return strdup("");
    }
    return buf.data;
}

int drift_check(const agent_config_t *config, char **explanation_out) {
    if (!saved_original_prompt || !config) {
        return 0;
    }

    // Get current modified files
    char *current_files = exec_command("git diff --name-only");
    if (!current_files || strlen(current_files) == 0) {
        free(current_files);
        return 0; // No changes, no drift possible
    }

    // If the changed files are identical to the last check, skip to save API calls
    if (last_checked_files && strcmp(last_checked_files, current_files) == 0) {
        free(current_files);
        return 0;
    }

    // Update last checked files
    if (last_checked_files) free(last_checked_files);
    last_checked_files = current_files; // take ownership

    // Get git diff
    char *diff_content = exec_command("git diff");
    if (!diff_content) {
        return 0;
    }

    // Truncate diff if too large for LLM call
    size_t max_diff_len = 8000;
    if (strlen(diff_content) > max_diff_len) {
        diff_content[max_diff_len] = '\0';
        strcat(diff_content, "\n... [diff truncated for size] ...");
    }

    // Request drift evaluation
    cJSON *request = cJSON_CreateObject();
    if (!request) {
        free(diff_content);
        return 0;
    }

    cJSON_AddStringToObject(request, "model", config->model);

    cJSON *messages = cJSON_CreateArray();
    cJSON *sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content",
        "You are a Goal Drift Detector. Your task is to compare the user's original task description with the actual code modifications (git diff) currently made by the AI agent.\n"
        "Determine if the agent has drifted from the task (e.g. refactoring unrelated modules, fixing unrelated bugs, expanding scope unnecessarily) or if the changes align with the original request.\n"
        "Respond ONLY with a JSON object in this format:\n"
        "{\n"
        "  \"drifted\": true/false,\n"
        "  \"explanation\": \"A short explanation of why the current changes are expanding the scope beyond the original goal, or why they are aligned.\"\n"
        "}\n"
        "Do not include any markdown wrappers or text outside the JSON.");
    cJSON_AddItemToArray(messages, sys_msg);

    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");

    // Format user payload
    char *user_payload = malloc(strlen(saved_original_prompt) + strlen(last_checked_files) + strlen(diff_content) + 512);
    if (user_payload) {
        sprintf(user_payload,
                "Original Goal: %s\n\n"
                "Modified Files:\n%s\n\n"
                "Git Diff:\n%s",
                saved_original_prompt, last_checked_files, diff_content);
        cJSON_AddStringToObject(user_msg, "content", user_payload);
        free(user_payload);
    } else {
        cJSON_AddStringToObject(user_msg, "content", "Out of memory");
    }
    cJSON_AddItemToArray(messages, user_msg);
    cJSON_AddItemToObject(request, "messages", messages);

    cJSON *response = NULL;
    int status = provider_chat_completion(config, request, &response);
    cJSON_Delete(request);
    free(diff_content);

    if (status != 0 || !response) {
        return 0; // Failed to call, assume no drift for safety
    }

    cJSON *choices = cJSON_GetObjectItemCaseSensitive(response, "choices");
    if (!cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        cJSON_Delete(response);
        return 0;
    }

    cJSON *first_choice = cJSON_GetArrayItem(choices, 0);
    cJSON *msg = cJSON_GetObjectItemCaseSensitive(first_choice, "message");
    cJSON *content = cJSON_GetObjectItemCaseSensitive(msg, "content");

    int drifted = 0;
    if (cJSON_IsString(content) && strlen(content->valuestring) > 0) {
        // Strip markdown if present
        const char *json_text = content->valuestring;
        const char *start = strchr(json_text, '{');
        if (start) {
            char *clean_json = strdup(start);
            char *end = strrchr(clean_json, '}');
            if (end) {
                *(end + 1) = '\0';
                cJSON *drift_result = cJSON_Parse(clean_json);
                if (drift_result) {
                    cJSON *drifted_item = cJSON_GetObjectItemCaseSensitive(drift_result, "drifted");
                    cJSON *exp_item = cJSON_GetObjectItemCaseSensitive(drift_result, "explanation");
                    
                    if (cJSON_IsTrue(drifted_item)) {
                        drifted = 1;
                        if (explanation_out && cJSON_IsString(exp_item)) {
                            *explanation_out = strdup(exp_item->valuestring);
                        }
                    }
                    cJSON_Delete(drift_result);
                }
            }
            free(clean_json);
        }
    }

    cJSON_Delete(response);
    return drifted;
}

int drift_handle_prompt(const agent_config_t *config, const char *explanation) {
    printf("\n\033[1;33m⚠️  [GOAL DRIFT DETECTED]\033[0m\n");
    printf("The agent's changes appear to be expanding beyond the original scope.\n");
    printf("Original Goal: %s\n", saved_original_prompt ? saved_original_prompt : "Unknown");
    printf("Reason: %s\n\n", explanation ? explanation : "Scope expansion");
    printf("Options:\n");
    printf("  \033[1;32m[c]\033[0m Continue anyway (proceed with changes)\n");
    printf("  \033[1;31m[r]\033[0m Revert to original scope (discard uncommitted files)\n");
    printf("  \033[1;34m[p]\033[0m Pause (stop current run, return to REPL prompt)\n");
    printf("Select option (c/r/p): ");
    fflush(stdout);

    char choice[32];
    if (fgets(choice, sizeof(choice), stdin)) {
        char ch = choice[0];
        if (ch == 'r' || ch == 'R') {
            printf("\n\033[1;31mDiscarding uncommitted changes...\033[0m\n");
            char *git_out = exec_command("git checkout -- . && git clean -fd");
            if (git_out) free(git_out);
            drift_reset();
            return 1;
        } else if (ch == 'p' || ch == 'P') {
            printf("\nPausing execution loop...\n");
            return 2;
        }
    }

    printf("\nContinuing execution...\n");
    return 0;
}
