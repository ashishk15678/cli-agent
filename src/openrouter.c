#include "openrouter.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int openrouter_chat_completion(const char *base_url, const char *api_key, const cJSON *request, cJSON **response_out) {
    if (!base_url || !*base_url || !api_key || !*api_key || !request || !response_out) {
        return 1;
    }

    char url[512];
    char auth_header[512];

    snprintf(url, sizeof(url), "%s/chat/completions", base_url);
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);

    char *body = cJSON_PrintUnformatted((cJSON *)request);
    if (!body) {
        fprintf(stderr, "failed to serialize request\n");
        return 1;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "failed to initialize curl\n");
        free(body);
        return 1;
    }

    struct response_buf resp = {NULL, 0};
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth_header);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

    CURLcode res = curl_easy_perform(curl);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body);

    if (res != CURLE_OK) {
        fprintf(stderr, "curl error: %s\n", curl_easy_strerror(res));
        free(resp.data);
        return 1;
    }

    *response_out = cJSON_Parse(resp.data ? resp.data : "");
    free(resp.data);
    if (!*response_out) {
        fprintf(stderr, "failed to parse response JSON\n");
        return 1;
    }

    return 0;
}
