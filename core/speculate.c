#include "speculate.h"
#include "provider.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>

#define SPEC_DIR ".sage/speculative"

static char *exec_in_dir(const char *command, const char *dir) {
    char full_cmd[8192];
    if (dir) {
        snprintf(full_cmd, sizeof(full_cmd), "cd %s && %s", dir, command);
    } else {
        snprintf(full_cmd, sizeof(full_cmd), "%s", command);
    }

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
        dup2(pipefd[1], STDERR_FILENO); // capture error output as well
        close(pipefd[0]);
        close(pipefd[1]);
        execl("/bin/bash", "bash", "-c", full_cmd, (char *)NULL);
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
    int status;
    waitpid(pid, &status, 0);

    if (!buf.data) {
        return strdup("");
    }
    return buf.data;
}

static int run_in_dir_exit_status(const char *command, const char *dir) {
    char full_cmd[8192];
    if (dir) {
        snprintf(full_cmd, sizeof(full_cmd), "cd %s && %s", dir, command);
    } else {
        snprintf(full_cmd, sizeof(full_cmd), "%s", command);
    }

    int status = system(full_cmd);
    if (status == -1) return -1;
    return WEXITSTATUS(status);
}

int speculate_run(const agent_config_t *config, int num_branches, const char **branch_prompts, const char *test_command) {
    if (num_branches < 1 || num_branches > 3 || !branch_prompts || !config) {
        fprintf(stderr, "Invalid speculative execution parameters\n");
        return 1;
    }

    printf("\n\033[1;35m🚀 Starting Multi-Branch Speculative Execution (%d branches simultaneously)...\033[0m\n", num_branches);
    
    // Clean and recreate speculative directory
    char *rm_out = exec_in_dir("rm -rf " SPEC_DIR " && mkdir -p " SPEC_DIR, NULL);
    if (rm_out) free(rm_out);

    char *branch_dirs[3] = {
        SPEC_DIR "/branch_1",
        SPEC_DIR "/branch_2",
        SPEC_DIR "/branch_3"
    };

    char *diffs[3] = {NULL, NULL, NULL};
    int compile_ok[3] = {0, 0, 0};
    int tests_ok[3] = {0, 0, 0};
    int scores[3] = {0, 0, 0};

    const char *sage_exec = "../../../build/sage";

    // Setup all sandboxes sequentially first
    for (int i = 0; i < num_branches; i++) {
        printf("\033[1;36m[Branch %d]\033[0m Preparing sandbox in '%s'...\n", i + 1, branch_dirs[i]);
        
        char mkdir_cmd[256];
        snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p %s", branch_dirs[i]);
        free(exec_in_dir(mkdir_cmd, NULL));

        char cp_cmd[1024];
        snprintf(cp_cmd, sizeof(cp_cmd), "cp -r CMakeLists.txt core include %s/", branch_dirs[i]);
        free(exec_in_dir(cp_cmd, NULL));

        free(exec_in_dir("git init && git config user.name \"sage-spec\" && git config user.email \"sage-spec@sage.ai\" && git add . && git commit -m \"initial\"", branch_dirs[i]));
    }

    // Build the option string based on active config
    char prov_opt[64] = {0};
    switch (config->provider) {
        case PROVIDER_OPENROUTER: strcpy(prov_opt, "-r openrouter"); break;
        case PROVIDER_OPENAI: strcpy(prov_opt, "-r openai"); break;
        case PROVIDER_ANTHROPIC: strcpy(prov_opt, "-r anthropic"); break;
        case PROVIDER_GEMINI: strcpy(prov_opt, "-r gemini"); break;
        case PROVIDER_GROQ: strcpy(prov_opt, "-r groq"); break;
        case PROVIDER_OLLAMA: strcpy(prov_opt, "-r ollama"); break;
    }

    char key_opt[1024] = {0};
    if (config->api_key && strlen(config->api_key) > 0) {
        snprintf(key_opt, sizeof(key_opt), "-k \"%s\"", config->api_key);
    }

    char model_opt[256] = {0};
    if (config->model && strlen(config->model) > 0) {
        snprintf(model_opt, sizeof(model_opt), "-m %s", config->model);
    }

    char url_opt[512] = {0};
    if (config->base_url && strlen(config->base_url) > 0) {
        snprintf(url_opt, sizeof(url_opt), "-u %s", config->base_url);
    }

    // Spawn all agent processes in parallel
    pid_t pids[3] = {0, 0, 0};
    printf("\n\033[1;35m🔥 Spawning all speculative agents simultaneously...\033[0m\n");
    fflush(stdout);

    for (int i = 0; i < num_branches; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "Failed to fork process for Branch %d\n", i + 1);
        } else if (pid == 0) {
            // Child process: run the agent command redirecting output to log file
            char agent_cmd[8192];
            snprintf(agent_cmd, sizeof(agent_cmd), "%s -p \"%s\" %s %s %s %s &> agent.log",
                     sage_exec, branch_prompts[i], prov_opt, model_opt, key_opt, url_opt);
            
            int exit_code = run_in_dir_exit_status(agent_cmd, branch_dirs[i]);
            _exit(exit_code);
        } else {
            pids[i] = pid;
            printf("\033[1;36m[Branch %d]\033[0m Started parallel agent process (PID: %d)...\n", i + 1, pid);
        }
    }

    // Wait for all agents to complete
    printf("Waiting for parallel agent runs to complete...\n");
    fflush(stdout);
    for (int i = 0; i < num_branches; i++) {
        if (pids[i] > 0) {
            int status = 0;
            waitpid(pids[i], &status, 0);
            printf("\033[1;36m[Branch %d]\033[0m Agent finished (Exit Code: %d)\n", i + 1, WEXITSTATUS(status));
        }
    }

    // Run build and tests in parallel for all sandboxes
    pid_t test_pids[3] = {0, 0, 0};
    printf("\n\033[1;35m🛠️  Running builds and test suites simultaneously in parallel...\033[0m\n");
    fflush(stdout);

    for (int i = 0; i < num_branches; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "Failed to fork build process for Branch %d\n", i + 1);
        } else if (pid == 0) {
            // Child process: compile and test
            char build_cmd[1024];
            if (test_command && strlen(test_command) > 0) {
                snprintf(build_cmd, sizeof(build_cmd), "(cmake -B build -S . && cmake --build build && %s) &> build.log", test_command);
            } else {
                snprintf(build_cmd, sizeof(build_cmd), "(cmake -B build -S . && cmake --build build) &> build.log");
            }
            int exit_code = run_in_dir_exit_status(build_cmd, branch_dirs[i]);
            _exit(exit_code);
        } else {
            test_pids[i] = pid;
        }
    }

    // Wait for all tests/builds to complete
    for (int i = 0; i < num_branches; i++) {
        if (test_pids[i] > 0) {
            int status = 0;
            waitpid(test_pids[i], &status, 0);
            int exit_code = WEXITSTATUS(status);
            if (exit_code == 0) {
                compile_ok[i] = 1;
                tests_ok[i] = 1;
                printf("\033[1;36m[Branch %d]\033[0m Build & Tests: \033[1;32mPASSED\033[0m\n", i + 1);
            } else {
                compile_ok[i] = 0;
                tests_ok[i] = 0;
                printf("\033[1;36m[Branch %d]\033[0m Build & Tests: \033[1;31mFAILED\033[0m (exit status: %d)\n", i + 1, exit_code);
            }
        }
        
        // Capture diff
        diffs[i] = exec_in_dir("git diff", branch_dirs[i]);
    }

    // Ask the LLM to score the branches
    printf("\n\033[1;35m🤔 Scoring branches via LLM...\033[0m\n");
    fflush(stdout);

    cJSON *request = cJSON_CreateObject();
    if (!request) {
        for (int i = 0; i < num_branches; i++) if (diffs[i]) free(diffs[i]);
        return 1;
    }

    cJSON_AddStringToObject(request, "model", config->model);

    cJSON *messages = cJSON_CreateArray();
    cJSON *sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content",
        "You are an expert code architect. You are reviewing multiple alternative speculative implementations of a task.\n"
        "Analyze the code changes (git diffs), compilation status, and test execution status for each branch.\n"
        "Score each branch from 0 to 100 based on code quality, completeness, style, type-safety, and test results.\n"
        "Respond ONLY with a JSON object in this format:\n"
        "{\n"
        "  \"branch_scores\": [85, 90, 75],\n"
        "  \"reason\": \"A short summary of why the winning branch was chosen.\"\n"
        "}\n"
        "Do not include markdown wrappers or text outside the JSON.");
    cJSON_AddItemToArray(messages, sys_msg);

    cJSON *user_msg = cJSON_CreateObject();
    struct string_buf sb = {NULL, 0};
    string_buf_append(&sb, "Evaluate the speculative implementations:\n\n", 43);

    for (int i = 0; i < num_branches; i++) {
        char header[256];
        snprintf(header, sizeof(header), "--- BRANCH %d ---\nPrompt: %s\nCompile: %s, Tests: %s\nDiff:\n",
                 i + 1, branch_prompts[i],
                 compile_ok[i] ? "OK" : "FAILED",
                 tests_ok[i] ? "OK" : "FAILED");
        string_buf_append(&sb, header, strlen(header));
        
        if (diffs[i] && strlen(diffs[i]) > 0) {
            size_t max_diff = 4000;
            if (strlen(diffs[i]) > max_diff) {
                string_buf_append(&sb, diffs[i], max_diff);
                string_buf_append(&sb, "\n... [diff truncated] ...\n", 26);
            } else {
                string_buf_append(&sb, diffs[i], strlen(diffs[i]));
            }
        } else {
            string_buf_append(&sb, "(No changes)\n", 13);
        }
        string_buf_append(&sb, "\n\n", 2);
    }

    cJSON_AddStringToObject(user_msg, "content", sb.data);
    free(sb.data);
    cJSON_AddItemToArray(messages, user_msg);
    cJSON_AddItemToObject(request, "messages", messages);

    cJSON *response = NULL;
    int status = provider_chat_completion(config, request, &response);
    cJSON_Delete(request);

    int winner_index = 0;
    int max_score = -1;
    char *reason = NULL;

    if (status == 0 && response) {
        cJSON *choices = cJSON_GetObjectItemCaseSensitive(response, "choices");
        if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
            cJSON *first_choice = cJSON_GetArrayItem(choices, 0);
            cJSON *msg = cJSON_GetObjectItemCaseSensitive(first_choice, "message");
            cJSON *content = cJSON_GetObjectItemCaseSensitive(msg, "content");
            if (cJSON_IsString(content)) {
                const char *json_text = content->valuestring;
                const char *start = strchr(json_text, '{');
                if (start) {
                    char *clean_json = strdup(start);
                    char *end = strrchr(clean_json, '}');
                    if (end) {
                        *(end + 1) = '\0';
                        cJSON *score_result = cJSON_Parse(clean_json);
                        if (score_result) {
                            cJSON *scores_arr = cJSON_GetObjectItemCaseSensitive(score_result, "branch_scores");
                            cJSON *reason_item = cJSON_GetObjectItemCaseSensitive(score_result, "reason");
                            
                            if (cJSON_IsArray(scores_arr)) {
                                int s_count = cJSON_GetArraySize(scores_arr);
                                for (int i = 0; i < s_count && i < num_branches; i++) {
                                    scores[i] = cJSON_GetArrayItem(scores_arr, i)->valueint;
                                }
                            }
                            if (cJSON_IsString(reason_item)) {
                                reason = strdup(reason_item->valuestring);
                            }
                            cJSON_Delete(score_result);
                        }
                    }
                    free(clean_json);
                }
            }
        }
        cJSON_Delete(response);
    }

    // Adjust scores based on compile and test status
    for (int i = 0; i < num_branches; i++) {
        if (!compile_ok[i]) scores[i] -= 100;
        if (!tests_ok[i]) scores[i] -= 100;
        
        printf("Branch %d Score: %d\n", i + 1, scores[i]);
        if (scores[i] > max_score) {
            max_score = scores[i];
            winner_index = i;
        }
    }

    printf("\n\033[1;32m🏆 Winner Chosen: Branch %d\033[0m\n", winner_index + 1);
    if (reason) {
        printf("Reason: %s\n", reason);
        free(reason);
    }

    // Apply the winning branch's changes back to the workspace
    printf("\nApplying Branch %d changes to main workspace...\n", winner_index + 1);
    fflush(stdout);

    // Copy the contents of core/ and include/ from winning sandbox back to root
    char merge_cmd[2048];
    snprintf(merge_cmd, sizeof(merge_cmd), "cp -r %s/core/* core/ && cp -r %s/include/* include/", 
             branch_dirs[winner_index], branch_dirs[winner_index]);
    free(exec_in_dir(merge_cmd, NULL));

    // Cleanup speculative dirs
    free(exec_in_dir("rm -rf " SPEC_DIR, NULL));
    for (int i = 0; i < num_branches; i++) if (diffs[i]) free(diffs[i]);

    printf("\033[1;32m✔ Merged successfully! Speculative run completed.\033[0m\n\n");
    return 0;
}
