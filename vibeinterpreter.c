#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>

#define API_KEY_PATH "/etc/vibeinterpreter/api.secret"
#define API_URL_FMT "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent?key=%s"
#define MAX_SCRIPT_SIZE (1024 * 64)
#define MAX_RESPONSE_SIZE (1024 * 256)

struct buffer {
    char *data;
    size_t size;
};

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    size_t total = size * nmemb;
    struct buffer *buf = userdata;

    char *tmp = realloc(buf->data, buf->size + total + 1);
    if (!tmp)
        return 0;

    buf->data = tmp;
    memcpy(buf->data + buf->size, ptr, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}

static char *read_file_strip(const char *path, int skip_shebang)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        perror(path);
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    rewind(fp);

    if (len <= 0 || len > MAX_SCRIPT_SIZE) {
        fprintf(stderr, "vibeinterpreter: script too large or empty\n");
        fclose(fp);
        return NULL;
    }

    char *buf = malloc(len + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }

    size_t nread = fread(buf, 1, len, fp);
    (void)nread;
    buf[len] = '\0';
    fclose(fp);

    if (skip_shebang && buf[0] == '#' && buf[1] == '!') {
        char *nl = strchr(buf, '\n');
        if (nl) {
            char *stripped = strdup(nl + 1);
            free(buf);
            return stripped;
        }
    }
    return buf;
}

static char *read_api_key(void)
{
    char *key = read_file_strip(API_KEY_PATH, 0);
    if (!key)
        return NULL;

    /* trim trailing whitespace */
    size_t len = strlen(key);
    while (len > 0 && (key[len - 1] == '\n' || key[len - 1] == '\r' ||
                        key[len - 1] == ' ' || key[len - 1] == '\t'))
        key[--len] = '\0';

    if (len == 0) {
        fprintf(stderr, "vibeinterpreter: empty API key in %s\n", API_KEY_PATH);
        free(key);
        return NULL;
    }
    return key;
}

/* Minimal JSON string escaper */
static char *json_escape(const char *src)
{
    size_t len = strlen(src);
    /* worst case: every char needs escaping */
    char *out = malloc(len * 6 + 1);
    if (!out)
        return NULL;

    char *p = out;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = src[i];
        switch (c) {
        case '"':  *p++ = '\\'; *p++ = '"';  break;
        case '\\': *p++ = '\\'; *p++ = '\\'; break;
        case '\n': *p++ = '\\'; *p++ = 'n';  break;
        case '\r': *p++ = '\\'; *p++ = 'r';  break;
        case '\t': *p++ = '\\'; *p++ = 't';  break;
        default:
            if (c < 0x20) {
                p += sprintf(p, "\\u%04x", c);
            } else {
                *p++ = c;
            }
        }
    }
    *p = '\0';
    return out;
}

/*
 * Extract text from Gemini API JSON response.
 * Looks for: "text": "..." in candidates[0].content.parts[0].
 */
static char *extract_text(const char *json)
{
    /* Find "parts" first to skip past any "text" in other fields */
    const char *parts = strstr(json, "\"parts\"");
    if (!parts)
        return NULL;

    const char *needle = "\"text\"";
    const char *pos = strstr(parts, needle);
    if (!pos)
        return NULL;

    pos += strlen(needle);
    while (*pos == ' ' || *pos == ':' || *pos == '\t' || *pos == '\n')
        pos++;

    if (*pos != '"')
        return NULL;
    pos++;

    size_t cap = 4096;
    char *out = malloc(cap);
    if (!out)
        return NULL;

    size_t len = 0;
    while (*pos && *pos != '"') {
        if (len + 4 >= cap) {
            cap *= 2;
            char *tmp = realloc(out, cap);
            if (!tmp) { free(out); return NULL; }
            out = tmp;
        }
        if (*pos == '\\') {
            pos++;
            switch (*pos) {
            case '"':  out[len++] = '"';  break;
            case '\\': out[len++] = '\\'; break;
            case 'n':  out[len++] = '\n'; break;
            case 'r':  out[len++] = '\r'; break;
            case 't':  out[len++] = '\t'; break;
            case '/':  out[len++] = '/';  break;
            default:   out[len++] = *pos; break;
            }
        } else {
            out[len++] = *pos;
        }
        pos++;
    }
    out[len] = '\0';
    return out;
}

/* Check for API error in response */
static int check_api_error(const char *json)
{
    const char *err = strstr(json, "\"error\"");
    if (!err)
        return 0;

    const char *msg = strstr(err, "\"message\"");
    if (msg) {
        msg = strchr(msg + 9, '"');
        if (msg) {
            msg++;
            const char *end = strchr(msg, '"');
            if (end) {
                fprintf(stderr, "vibeinterpreter: API error: %.*s\n",
                        (int)(end - msg), msg);
                return 1;
            }
        }
    }
    fprintf(stderr, "vibeinterpreter: API returned an error\n");
    return 1;
}

static char *call_llm(const char *api_key, const char *prompt)
{
    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "vibeinterpreter: curl init failed\n");
        return NULL;
    }

    char *escaped = json_escape(prompt);
    if (!escaped) {
        curl_easy_cleanup(curl);
        return NULL;
    }

    const char *system_prompt =
        "You are a natural language to bash converter. "
        "Convert the user's natural language instructions into a bash script. "
        "Output ONLY valid bash code with no markdown fences, no explanations, "
        "no comments unless necessary for correctness. "
        "The output must be directly executable by /bin/bash.";

    char *sys_escaped = json_escape(system_prompt);

    size_t body_len = strlen(escaped) + strlen(sys_escaped) + 512;
    char *body = malloc(body_len);
    snprintf(body, body_len,
        "{\"system_instruction\":{\"parts\":[{\"text\":\"%s\"}]},"
        "\"contents\":[{\"parts\":[{\"text\":\"%s\"}]}]}",
        sys_escaped, escaped);

    free(escaped);
    free(sys_escaped);

    /* Build URL with API key as query parameter */
    size_t url_len = strlen(api_key) + 256;
    char *url = malloc(url_len);
    snprintf(url, url_len, API_URL_FMT, api_key);

    struct buffer resp = { .data = malloc(1), .size = 0 };
    resp.data[0] = '\0';

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "content-type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

    CURLcode res = curl_easy_perform(curl);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body);
    free(url);

    if (res != CURLE_OK) {
        fprintf(stderr, "vibeinterpreter: HTTP request failed: %s\n",
                curl_easy_strerror(res));
        free(resp.data);
        return NULL;
    }

    if (check_api_error(resp.data)) {
        free(resp.data);
        return NULL;
    }

    char *bash = extract_text(resp.data);
    free(resp.data);

    if (!bash) {
        fprintf(stderr, "vibeinterpreter: failed to parse LLM response\n");
        return NULL;
    }
    return bash;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: vibeinterpreter <script.vi>\n");
        return 1;
    }

    char *script = read_file_strip(argv[1], 1);
    if (!script)
        return 1;

    if (strlen(script) == 0) {
        fprintf(stderr, "vibeinterpreter: empty script\n");
        free(script);
        return 1;
    }

    char *api_key = read_api_key();
    if (!api_key) {
        free(script);
        return 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    char *bash = call_llm(api_key, script);
    free(script);
    free(api_key);

    curl_global_cleanup();

    if (!bash)
        return 1;

    /* Write bash to a temp file and execute it */
    char tmppath[] = "/tmp/vibeinterpreter.XXXXXX";
    int fd = mkstemp(tmppath);
    if (fd < 0) {
        perror("mkstemp");
        free(bash);
        return 1;
    }

    if (write(fd, "#!/bin/bash\n", 12) < 0 ||
        write(fd, bash, strlen(bash)) < 0 ||
        write(fd, "\n", 1) < 0) {
        perror("write");
        close(fd);
        unlink(tmppath);
        free(bash);
        return 1;
    }
    close(fd);
    free(bash);

    /* Execute via bash, passing through any extra arguments */
    /* argv: bash, tmppath, [script args...], NULL */
    int extra_args = argc - 2;
    char **exec_argv = malloc(sizeof(char *) * (extra_args + 3));
    exec_argv[0] = "/bin/bash";
    exec_argv[1] = tmppath;
    for (int i = 0; i < extra_args; i++)
        exec_argv[i + 2] = argv[i + 2];
    exec_argv[extra_args + 2] = NULL;

    execv("/bin/bash", exec_argv);

    /* only reached on exec failure */
    perror("execv");
    unlink(tmppath);
    free(exec_argv);
    return 1;
}
