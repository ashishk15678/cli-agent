#include "memory.h"
#include "utils.h"
#include "provider.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <math.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>

#define MEMORY_DIR ".sage"
#define MEMORY_FILE ".sage/memory.json"

static cJSON *memory_db = NULL;

struct response_buf {
    char *data;
    size_t size;
};

static size_t curl_write_response(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    struct response_buf *buf = (struct response_buf *)userp;
    char *tmp = realloc(buf->data, buf->size + total + 1);
    if (!tmp) return 0;
    buf->data = tmp;
    memcpy(buf->data + buf->size, contents, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}

// Convert string to lowercase
static char *lowercase_dup(const char *str) {
    if (!str) return NULL;
    char *dup = strdup(str);
    if (!dup) return NULL;
    for (int i = 0; dup[i]; i++) {
        dup[i] = tolower((unsigned char)dup[i]);
    }
    return dup;
}

// Simple stopword check
static int is_stopword(const char *word) {
    const char *stopwords[] = {
        "the", "a", "an", "and", "or", "but", "if", "then", "else", "to", "for",
        "in", "on", "at", "by", "from", "with", "about", "as", "of", "that", "this",
        "these", "those", "is", "are", "was", "were", "be", "been", "being",
        "have", "has", "had", "do", "does", "did", "we", "you", "they", "he", "she",
        "it", "i", "always", "always", "never", "use", "using", "uses", "we", "our",
        "my", "me", "us"
    };
    int num_stopwords = sizeof(stopwords) / sizeof(stopwords[0]);
    for (int i = 0; i < num_stopwords; i++) {
        if (strcmp(word, stopwords[i]) == 0) return 1;
    }
    return 0;
}

// Compute simple word overlap similarity (Jaccard-like index on content words)
static double keyword_similarity(const char *s1, const char *s2) {
    char *l1 = lowercase_dup(s1);
    char *l2 = lowercase_dup(s2);
    if (!l1 || !l2) {
        free(l1);
        free(l2);
        return 0.0;
    }

    // Tokenize s1
    char **w1 = NULL;
    int count1 = 0;
    char *token = strtok(l1, " \t\r\n.,;!?-()[]{}\"'/\\_@*+=:");
    while (token) {
        if (strlen(token) > 2 && !is_stopword(token)) {
            // check unique
            int found = 0;
            for (int i = 0; i < count1; i++) {
                if (strcmp(w1[i], token) == 0) { found = 1; break; }
            }
            if (!found) {
                w1 = realloc(w1, sizeof(char*) * (count1 + 1));
                w1[count1++] = strdup(token);
            }
        }
        token = strtok(NULL, " \t\r\n.,;!?-()[]{}\"'/\\_@*+=:");
    }

    // Tokenize s2
    char **w2 = NULL;
    int count2 = 0;
    token = strtok(l2, " \t\r\n.,;!?-()[]{}\"'/\\_@*+=:");
    while (token) {
        if (strlen(token) > 2 && !is_stopword(token)) {
            int found = 0;
            for (int i = 0; i < count2; i++) {
                if (strcmp(w2[i], token) == 0) { found = 1; break; }
            }
            if (!found) {
                w2 = realloc(w2, sizeof(char*) * (count2 + 1));
                w2[count2++] = strdup(token);
            }
        }
        token = strtok(NULL, " \t\r\n.,;!?-()[]{}\"'/\\_@*+=:");
    }

    if (count1 == 0 || count2 == 0) {
        // cleanup
        for (int i = 0; i < count1; i++) free(w1[i]);
        free(w1);
        for (int i = 0; i < count2; i++) free(w2[i]);
        free(w2);
        free(l1);
        free(l2);
        return 0.0;
    }

    // Count intersection
    int intersection = 0;
    for (int i = 0; i < count1; i++) {
        for (int j = 0; j < count2; j++) {
            if (strcmp(w1[i], w2[j]) == 0) {
                intersection++;
                break;
            }
        }
    }

    double score = (double)intersection / (count1 + count2 - intersection);

    // cleanup
    for (int i = 0; i < count1; i++) free(w1[i]);
    free(w1);
    for (int i = 0; i < count2; i++) free(w2[i]);
    free(w2);
    free(l1);
    free(l2);

    return score;
}

// Retrieve embedding vector from LLM provider
static double *fetch_embedding(const agent_config_t *config, const char *text, int *dim_out) {
    if (!config || !text || !config->api_key || strlen(config->api_key) == 0) {
        return NULL;
    }

    char url[1024];
    cJSON *payload = cJSON_CreateObject();
    if (!payload) return NULL;

    CURL *curl = curl_easy_init();
    if (!curl) {
        cJSON_Delete(payload);
        return NULL;
    }

    struct response_buf resp = {NULL, 0};
    struct curl_slist *headers = NULL;

    if (config->provider == PROVIDER_GEMINI) {
        snprintf(url, sizeof(url), "https://generativelanguage.googleapis.com/v1/models/text-embedding-004:embedContent");
        
        cJSON *content = cJSON_CreateObject();
        cJSON *parts = cJSON_CreateArray();
        cJSON *part = cJSON_CreateObject();
        cJSON_AddStringToObject(part, "text", text);
        cJSON_AddItemToArray(parts, part);
        cJSON_AddItemToObject(content, "parts", parts);
        cJSON_AddItemToObject(payload, "content", content);

        headers = curl_slist_append(headers, "Content-Type: application/json");
        char key_hdr[256];
        snprintf(key_hdr, sizeof(key_hdr), "x-goog-api-key: %s", config->api_key);
        headers = curl_slist_append(headers, key_hdr);
    } else if (config->provider == PROVIDER_OLLAMA) {
        // Default local Ollama URL
        const char *base = config->base_url && strlen(config->base_url) > 0 ? config->base_url : "http://localhost:11434";
        snprintf(url, sizeof(url), "%s/api/embeddings", base);
        
        cJSON_AddStringToObject(payload, "model", "nomic-embed-text");
        cJSON_AddStringToObject(payload, "prompt", text);
        
        headers = curl_slist_append(headers, "Content-Type: application/json");
    } else {
        // OpenAI / OpenRouter / Anthropic (we map Anthropic to OpenRouter or OpenAI for embedding if they have keys)
        const char *base = (config->provider == PROVIDER_OPENROUTER) ? "https://openrouter.ai/api/v1" : 
                          (config->base_url && strlen(config->base_url) > 0 ? config->base_url : "https://api.openai.com/v1");
        snprintf(url, sizeof(url), "%s/embeddings", base);

        cJSON_AddStringToObject(payload, "model", "text-embedding-3-small");
        cJSON_AddStringToObject(payload, "input", text);

        headers = curl_slist_append(headers, "Content-Type: application/json");
        char auth_hdr[512];
        snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", config->api_key);
        headers = curl_slist_append(headers, auth_hdr);
    }

    char *body = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);

    if (!body) {
        curl_easy_cleanup(curl);
        return NULL;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    // Timeout embeddings quickly to prevent blocking
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body);

    if (res != CURLE_OK) {
        free(resp.data);
        return NULL;
    }

    cJSON *json = cJSON_Parse(resp.data ? resp.data : "");
    free(resp.data);
    if (!json) return NULL;

    cJSON *embedding_arr = NULL;
    if (config->provider == PROVIDER_GEMINI) {
        cJSON *embedding_obj = cJSON_GetObjectItemCaseSensitive(json, "embedding");
        embedding_arr = cJSON_GetObjectItemCaseSensitive(embedding_obj, "values");
    } else if (config->provider == PROVIDER_OLLAMA) {
        embedding_arr = cJSON_GetObjectItemCaseSensitive(json, "embedding");
    } else {
        cJSON *data_arr = cJSON_GetObjectItemCaseSensitive(json, "data");
        if (cJSON_IsArray(data_arr) && cJSON_GetArraySize(data_arr) > 0) {
            cJSON *first_item = cJSON_GetArrayItem(data_arr, 0);
            embedding_arr = cJSON_GetObjectItemCaseSensitive(first_item, "embedding");
        }
    }

    if (!cJSON_IsArray(embedding_arr)) {
        cJSON_Delete(json);
        return NULL;
    }

    int dim = cJSON_GetArraySize(embedding_arr);
    double *vector = malloc(dim * sizeof(double));
    if (!vector) {
        cJSON_Delete(json);
        return NULL;
    }

    for (int i = 0; i < dim; i++) {
        vector[i] = cJSON_GetArrayItem(embedding_arr, i)->valuedouble;
    }

    *dim_out = dim;
    cJSON_Delete(json);
    return vector;
}

// Compute cosine similarity between two vectors
static double cosine_similarity(const double *a, const double *b, int dim) {
    double dot = 0.0, denom_a = 0.0, denom_b = 0.0;
    for (int i = 0; i < dim; i++) {
        dot += a[i] * b[i];
        denom_a += a[i] * a[i];
        denom_b += b[i] * b[i];
    }
    if (denom_a == 0.0 || denom_b == 0.0) return 0.0;
    return dot / (sqrt(denom_a) * sqrt(denom_b));
}

// Helper: check if file exists
static int file_exists(const char *path) {
    struct stat buffer;
    return (stat(path, &buffer) == 0);
}

// Save memory DB to file
static int save_db(void) {
    if (!memory_db) return 0;
    ensure_parent_directories(MEMORY_FILE);
    char *serialized = cJSON_Print(memory_db);
    if (!serialized) return 1;

    if (write_entire_file(MEMORY_FILE, serialized) != 0) {
        free(serialized);
        return 1;
    }
    free(serialized);
    return 0;
}

int memory_init(const agent_config_t *config) {
    if (memory_db) {
        cJSON_Delete(memory_db);
        memory_db = NULL;
    }

    if (!file_exists(MEMORY_FILE)) {
        memory_db = cJSON_CreateArray();
        save_db();
        return 0;
    }

    char *contents = read_entire_file(MEMORY_FILE, NULL);
    if (!contents) {
        memory_db = cJSON_CreateArray();
        return 1;
    }

    memory_db = cJSON_Parse(contents);
    free(contents);

    if (!memory_db || !cJSON_IsArray(memory_db)) {
        if (memory_db) cJSON_Delete(memory_db);
        memory_db = cJSON_CreateArray();
    }

    return 0;
}

int memory_add(const agent_config_t *config, const char *text, const char *domain, int importance) {
    if (!memory_db) {
        memory_init(config);
    }

    cJSON *item = cJSON_CreateObject();
    if (!item) return 1;

    cJSON_AddStringToObject(item, "text", text);
    cJSON_AddStringToObject(item, "domain", domain ? domain : "general");
    cJSON_AddNumberToObject(item, "importance", importance);
    cJSON_AddNumberToObject(item, "timestamp", (double)time(NULL));

    // Try to fetch embedding
    int dim = 0;
    double *vector = fetch_embedding(config, text, &dim);
    if (vector) {
        cJSON *embedding_arr = cJSON_CreateArray();
        for (int i = 0; i < dim; i++) {
            cJSON_AddItemToArray(embedding_arr, cJSON_CreateNumber(vector[i]));
        }
        cJSON_AddItemToObject(item, "embedding", embedding_arr);
        free(vector);
    }

    cJSON_AddItemToArray(memory_db, item);
    save_db();
    return 0;
}

typedef struct {
    int index;
    double score;
} search_result_t;

static int compare_results(const void *a, const void *b) {
    double diff = ((search_result_t*)b)->score - ((search_result_t*)a)->score;
    return (diff > 0) ? 1 : ((diff < 0) ? -1 : 0);
}

int memory_retrieve_and_inject(const agent_config_t *config, const char *query_text, cJSON *messages) {
    if (!memory_db || cJSON_GetArraySize(memory_db) == 0) {
        return 0; // No memories to retrieve
    }

    int count = cJSON_GetArraySize(memory_db);
    search_result_t *results = malloc(count * sizeof(search_result_t));
    if (!results) return 1;

    // Fetch embedding of query
    int query_dim = 0;
    double *query_vector = fetch_embedding(config, query_text, &query_dim);

    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(memory_db, i);
        cJSON *text_item = cJSON_GetObjectItemCaseSensitive(item, "text");
        cJSON *emb_item = cJSON_GetObjectItemCaseSensitive(item, "embedding");
        cJSON *imp_item = cJSON_GetObjectItemCaseSensitive(item, "importance");
        cJSON *time_item = cJSON_GetObjectItemCaseSensitive(item, "timestamp");

        double importance = cJSON_IsNumber(imp_item) ? imp_item->valuedouble : 5.0;
        double timestamp = cJSON_IsNumber(time_item) ? time_item->valuedouble : 0.0;
        const char *text = cJSON_IsString(text_item) ? text_item->valuestring : "";

        double score = 0.0;

        if (query_vector && cJSON_IsArray(emb_item) && cJSON_GetArraySize(emb_item) == query_dim) {
            // Cosine similarity
            int dim = cJSON_GetArraySize(emb_item);
            double *vector = malloc(dim * sizeof(double));
            if (vector) {
                for (int j = 0; j < dim; j++) {
                    vector[j] = cJSON_GetArrayItem(emb_item, j)->valuedouble;
                }
                score = cosine_similarity(query_vector, vector, dim);
                free(vector);
            }
        } else {
            // Fallback: word overlap similarity
            score = keyword_similarity(query_text, text);
        }

        // Apply weights for recency and importance
        double age_days = (time(NULL) - timestamp) / 86400.0;
        double recency_weight = 1.0 / (1.0 + age_days * 0.05); // slowly decay older memories
        double importance_weight = importance / 10.0;

        results[i].index = i;
        results[i].score = score * recency_weight * importance_weight;
    }

    free(query_vector);

    // Sort by score descending
    qsort(results, count, sizeof(search_result_t), compare_results);

    // Get top N memories (up to 4)
    int retrieved_count = (count < 4) ? count : 4;
    int actual_added = 0;

    cJSON *memory_blocks = cJSON_CreateArray();

    for (int i = 0; i < retrieved_count; i++) {
        if (results[i].score <= 0.01) continue; // ignore non-relevant

        cJSON *item = cJSON_GetArrayItem(memory_db, results[i].index);
        cJSON *text_item = cJSON_GetObjectItemCaseSensitive(item, "text");
        cJSON *dom_item = cJSON_GetObjectItemCaseSensitive(item, "domain");
        if (cJSON_IsString(text_item)) {
            cJSON *block = cJSON_CreateObject();
            cJSON_AddStringToObject(block, "text", text_item->valuestring);
            cJSON_AddStringToObject(block, "domain", cJSON_IsString(dom_item) ? dom_item->valuestring : "general");
            cJSON_AddItemToArray(memory_blocks, block);
            actual_added++;
        }
    }

    free(results);

    if (actual_added > 0) {
        // Inject into the system prompt message at index 0, or prepend to the first user message
        cJSON *first_msg = cJSON_GetArrayItem(messages, 0);
        cJSON *role_item = cJSON_GetObjectItemCaseSensitive(first_msg, "role");
        cJSON *content_item = cJSON_GetObjectItemCaseSensitive(first_msg, "content");

        if (cJSON_IsString(role_item) && strcmp(role_item->valuestring, "system") == 0 && cJSON_IsString(content_item)) {
            // Prepend semantic memories to system instructions
            struct string_buf sb = {NULL, 0};
            string_buf_append(&sb, "=== PROJECT MEMORIES & CONVENTIONS ===\n", 39);
            int block_count = cJSON_GetArraySize(memory_blocks);
            for (int i = 0; i < block_count; i++) {
                cJSON *b = cJSON_GetArrayItem(memory_blocks, i);
                cJSON *txt = cJSON_GetObjectItemCaseSensitive(b, "text");
                cJSON *dom = cJSON_GetObjectItemCaseSensitive(b, "domain");
                char line[1024];
                snprintf(line, sizeof(line), "- [%s] %s\n", txt->valuestring, dom->valuestring);
                string_buf_append(&sb, line, strlen(line));
            }
            string_buf_append(&sb, "======================================\n\n", 40);
            string_buf_append(&sb, content_item->valuestring, strlen(content_item->valuestring));

            cJSON_SetValuestring(content_item, sb.data);
            free(sb.data);
        }
    }

    cJSON_Delete(memory_blocks);
    return 0;
}

int memory_auto_extract(const agent_config_t *config, const cJSON *messages) {
    if (!messages || cJSON_GetArraySize(messages) < 2) return 0;

    // Build a lightweight extraction query to the LLM
    // We send a request containing the recent conversation and ask for any decisions/rules
    cJSON *extraction_request = cJSON_CreateObject();
    if (!extraction_request) return 1;

    cJSON_AddStringToObject(extraction_request, "model", config->model);

    cJSON *messages_copy = cJSON_CreateArray();
    
    // Add instruction system message
    cJSON *sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", 
        "Analyze the following conversation between the user and AI coding assistant.\n"
        "Identify and extract any permanent technical decisions, project rules, coding conventions, API patterns, "
        "or directories/files to avoid that were agreed upon or specified.\n"
        "Ignore transient talk about debugging or minor steps. Focus on rules like 'this project uses Zod', "
        "'we use early returns', or 'always put tests in core/tests'.\n"
        "Format your answer as a JSON array of objects, each representing a single memory:\n"
        "[{\"text\": \"decision description\", \"domain\": \"category (e.g. testing/auth/validation/style)\", \"importance\": 1-10}]\n"
        "Respond ONLY with the JSON array, no markdown wrappers, no introductory or concluding text.");
    cJSON_AddItemToArray(messages_copy, sys_msg);

    // Copy the last few conversation turns to keep it cheap
    int count = cJSON_GetArraySize(messages);
    int start_idx = (count > 8) ? count - 8 : 1; // skip system prompt at 0
    for (int i = start_idx; i < count; i++) {
        cJSON *msg = cJSON_GetArrayItem(messages, i);
        cJSON *role = cJSON_GetObjectItemCaseSensitive(msg, "role");
        cJSON *content = cJSON_GetObjectItemCaseSensitive(msg, "content");
        if (cJSON_IsString(role) && cJSON_IsString(content) && strlen(content->valuestring) > 0) {
            // skip tool outputs to save context size
            if (strcmp(role->valuestring, "tool") == 0) continue;
            cJSON *msg_copy = cJSON_CreateObject();
            cJSON_AddStringToObject(msg_copy, "role", role->valuestring);
            cJSON_AddStringToObject(msg_copy, "content", content->valuestring);
            cJSON_AddItemToArray(messages_copy, msg_copy);
        }
    }

    cJSON_AddItemToObject(extraction_request, "messages", messages_copy);

    cJSON *response = NULL;
    int status = provider_chat_completion(config, extraction_request, &response);
    cJSON_Delete(extraction_request);

    if (status != 0 || !response) {
        return 1;
    }

    cJSON *choices = cJSON_GetObjectItemCaseSensitive(response, "choices");
    if (!cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        cJSON_Delete(response);
        return 1;
    }
    cJSON *first_choice = cJSON_GetArrayItem(choices, 0);
    cJSON *message = cJSON_GetObjectItemCaseSensitive(first_choice, "message");
    cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");

    if (cJSON_IsString(content) && strlen(content->valuestring) > 0) {
        // Strip markdown wrappers if LLM returned them
        const char *json_text = content->valuestring;
        const char *start = strchr(json_text, '[');
        if (start) {
            char *clean_json = strdup(start);
            char *end = strrchr(clean_json, ']');
            if (end) {
                *(end + 1) = '\0';
                cJSON *mem_arr = cJSON_Parse(clean_json);
                if (cJSON_IsArray(mem_arr)) {
                    int mem_count = cJSON_GetArraySize(mem_arr);
                    for (int i = 0; i < mem_count; i++) {
                        cJSON *m = cJSON_GetArrayItem(mem_arr, i);
                        cJSON *t = cJSON_GetObjectItemCaseSensitive(m, "text");
                        cJSON *d = cJSON_GetObjectItemCaseSensitive(m, "domain");
                        cJSON *imp = cJSON_GetObjectItemCaseSensitive(m, "importance");
                        if (cJSON_IsString(t)) {
                            memory_add(config, t->valuestring, 
                                       cJSON_IsString(d) ? d->valuestring : "general",
                                       cJSON_IsNumber(imp) ? imp->valueint : 5);
                            printf("\n\033[1;36m[Semantic Memory] Learned:\033[0m \"%s\" (Domain: %s)\n", 
                                   t->valuestring, cJSON_IsString(d) ? d->valuestring : "general");
                        }
                    }
                    cJSON_Delete(mem_arr);
                }
            }
            free(clean_json);
        }
    }

    cJSON_Delete(response);
    return 0;
}

void memory_print_all(void) {
    if (!memory_db || cJSON_GetArraySize(memory_db) == 0) {
        printf("No stored memories for this project.\n");
        return;
    }

    printf("\n--- Persistent Project Memories ---\n");
    int count = cJSON_GetArraySize(memory_db);
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(memory_db, i);
        cJSON *text = cJSON_GetObjectItemCaseSensitive(item, "text");
        cJSON *dom = cJSON_GetObjectItemCaseSensitive(item, "domain");
        cJSON *imp = cJSON_GetObjectItemCaseSensitive(item, "importance");
        cJSON *time_item = cJSON_GetObjectItemCaseSensitive(item, "timestamp");

        time_t timestamp = cJSON_IsNumber(time_item) ? (time_t)time_item->valuedouble : 0;
        char time_str[64] = {0};
        if (timestamp > 0) {
            struct tm *tm_info = localtime(&timestamp);
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M", tm_info);
        }

        printf("%d. [%s] %s (Importance: %d/10, Added: %s)\n",
               i + 1,
               cJSON_IsString(dom) ? dom->valuestring : "general",
               cJSON_IsString(text) ? text->valuestring : "",
               cJSON_IsNumber(imp) ? imp->valueint : 5,
               time_str);
    }
    printf("------------------------------------\n\n");
}
