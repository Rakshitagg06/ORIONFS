#include "utils.h"
#include <ctype.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h> // for unlink

// Global logger variables
static FILE* log_file = NULL;
static char component_name[64] = "SYSTEM";
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

// ==================== LOGGING IMPLEMENTATION ====================

void init_logger(const char* log_file_path, const char* comp_name) {
    log_file = fopen(log_file_path, "a");
    if (log_file == NULL) {
        fprintf(stderr, "Failed to open log file: %s\n", log_file_path);
        return;
    }
    strncpy(component_name, comp_name, sizeof(component_name) - 1);
    
    // Log initialization
    char timestamp[64];
    get_timestamp_string(timestamp, sizeof(timestamp));
    fprintf(log_file, "\n========== %s STARTED AT %s ==========\n", 
            component_name, timestamp);
    fflush(log_file);
}

const char* log_level_to_string(LogLevel level) {
    switch (level) {
        case LOG_DEBUG: return "DEBUG";
        case LOG_INFO: return "INFO";
        case LOG_WARNING: return "WARNING";
        case LOG_ERROR: return "ERROR";
        case LOG_CRITICAL: return "CRITICAL";
        default: return "UNKNOWN";
    }
}

void log_message(LogLevel level, const char* ip, int port, 
                 const char* username, const char* action, 
                 const char* details, const char* status) {
    if (log_file == NULL) return;
    
    pthread_mutex_lock(&log_mutex);
    
    char timestamp[64];
    get_timestamp_string(timestamp, sizeof(timestamp));
    
    fprintf(log_file, "[%s] [%s] [%s] [%s:%d] [%s] [%s] [%s] [%s]\n",
            timestamp, log_level_to_string(level), component_name,
            ip ? ip : "N/A", port, username ? username : "N/A",
            action ? action : "N/A", details ? details : "N/A",
            status ? status : "N/A");
    fflush(log_file);
    
    pthread_mutex_unlock(&log_mutex);
}

void close_logger() {
    if (log_file != NULL) {
        char timestamp[64];
        get_timestamp_string(timestamp, sizeof(timestamp));
        fprintf(log_file, "========== %s STOPPED AT %s ==========\n\n", 
                component_name, timestamp);
        fclose(log_file);
        log_file = NULL;
    }
}

// ==================== STRING UTILITIES ====================

char* trim_string(char* str) {
    if (str == NULL) return NULL;
    
    // Trim leading whitespace
    while (isspace((unsigned char)*str)) str++;
    
    if (*str == 0) return str;
    
    // Trim trailing whitespace
    char* end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    
    *(end + 1) = '\0';
    return str;
}

int split_string(char* str, char delimiter, char** tokens, int max_tokens) {
    int count = 0;
    char* token = strtok(str, &delimiter);
    
    while (token != NULL && count < max_tokens) {
        tokens[count++] = token;
        token = strtok(NULL, &delimiter);
    }
    
    return count;
}

bool is_sentence_delimiter(char c) {
    return (c == '.' || c == '!' || c == '?');
}

int parse_sentence_to_words(char* sentence, char** words, int max_words) {
    int count = 0;
    char* copy = strdup(sentence);
    char* token = strtok(copy, " \t\n");
    
    while (token != NULL && count < max_words) {
        words[count] = strdup(token);
        count++;
        token = strtok(NULL, " \t\n");
    }
    
    free(copy);
    return count;
}

void join_words_to_sentence(char** words, int word_count, char* sentence, int max_len) {
    sentence[0] = '\0';
    for (int i = 0; i < word_count; i++) {
        if (i > 0) strcat(sentence, " ");
        strncat(sentence, words[i], max_len - strlen(sentence) - 1);
    }
}

// ==================== TIME UTILITIES ====================

void get_timestamp_string(char* buffer, int buffer_size) {
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    strftime(buffer, buffer_size, "%Y-%m-%d %H:%M:%S", tm_info);
}

time_t get_current_timestamp() {
    return time(NULL);
}

void timestamp_to_string(time_t timestamp, char* buffer, int buffer_size) {
    struct tm* tm_info = localtime(&timestamp);
    strftime(buffer, buffer_size, "%Y-%m-%d %H:%M:%S", tm_info);
}

// ==================== FILE UTILITIES ====================

bool file_exists(const char* filepath) {
    struct stat st;
    return (stat(filepath, &st) == 0);
}

size_t get_file_size(const char* filepath) {
    struct stat st;
    if (stat(filepath, &st) == 0) {
        return st.st_size;
    }
    return 0;
}

int count_words_in_file(const char* filepath) {
    FILE* file = fopen(filepath, "r");
    if (file == NULL) return 0;
    
    int count = 0;
    char ch, prev = ' ';
    
    while ((ch = fgetc(file)) != EOF) {
        if (isspace(ch) && !isspace(prev)) {
            count++;
        }
        prev = ch;
    }
    
    if (!isspace(prev) && prev != EOF) count++;
    
    fclose(file);
    return count;
}

int count_chars_in_file(const char* filepath) {
    FILE* file = fopen(filepath, "r");
    if (file == NULL) return 0;
    
    fseek(file, 0, SEEK_END);
    int count = ftell(file);
    fclose(file);
    
    return count;
}

char* read_file_content(const char* filepath) {
    FILE* file = fopen(filepath, "r");
    if (file == NULL) return NULL;
    
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    char* content = (char*)malloc(file_size + 1);
    if (content == NULL) {
        fclose(file);
        return NULL;
    }
    
    fread(content, 1, file_size, file);
    content[file_size] = '\0';
    
    fclose(file);
    return content;
}

int write_file_content(const char* filepath, const char* content) {
    FILE* file = fopen(filepath, "w");
    if (file == NULL) return -1;
    
    fprintf(file, "%s", content);
    fclose(file);
    return 0;
}

int atomic_write_file(const char* filepath, const char* content) {
    char temp_file[MAX_FILENAME_LEN + 10];
    snprintf(temp_file, sizeof(temp_file), "%s.tmp", filepath);
    
    // Write to temp file
    if (write_file_content(temp_file, content) != 0) {
        return -1;
    }
    
    // Atomic rename
    if (rename(temp_file, filepath) != 0) {
        unlink(temp_file);
        return -1;
    }
    
    return 0;
}

// ==================== PARSING UTILITIES ====================

Command parse_command(char* input) {
    Command cmd = {0};
    cmd.arg_count = 0;
    cmd.flag_a = false;
    cmd.flag_l = false;
    cmd.flag_r = false;
    cmd.flag_w = false;
    
    char* tokens[20];
    char* input_copy = strdup(input);
    int token_count = split_string(input_copy, ' ', tokens, 20);
    
    if (token_count == 0) {
        free(input_copy);
        return cmd;
    }
    
    // First token is command
    strncpy(cmd.cmd, tokens[0], sizeof(cmd.cmd) - 1);
    
    // Parse remaining tokens
    for (int i = 1; i < token_count; i++) {
        char* token = tokens[i];
        
        // Check for flags
        if (token[0] == '-') {
            for (int j = 1; token[j] != '\0'; j++) {
                switch (token[j]) {
                    case 'a': cmd.flag_a = true; break;
                    case 'l': cmd.flag_l = true; break;
                    case 'R': cmd.flag_r = true; break;
                    case 'W': cmd.flag_w = true; break;
                }
            }
        } else {
            // Regular argument
            if (cmd.arg_count < 10) {
                strncpy(cmd.args[cmd.arg_count], token, MAX_FILENAME_LEN - 1);
                cmd.arg_count++;
            }
        }
    }
    
    free(input_copy);
    return cmd;
}

// ==================== VALIDATION ====================

bool is_valid_filename(const char* filename) {
    if (filename == NULL || strlen(filename) == 0 || strlen(filename) >= MAX_FILENAME_LEN) {
        return false;
    }
    
    // Check for invalid characters
    for (int i = 0; filename[i] != '\0'; i++) {
        char c = filename[i];
        if (c == '/' || c == '\\' || c == ':' || c == '*' || 
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            return false;
        }
    }
    
    return true;
}

bool is_valid_username(const char* username) {
    if (username == NULL || strlen(username) == 0 || strlen(username) >= MAX_USERNAME_LEN) {
        return false;
    }
    
    // Username should be alphanumeric (with underscore allowed)
    for (int i = 0; username[i] != '\0'; i++) {
        if (!isalnum(username[i]) && username[i] != '_') {
            return false;
        }
    }
    
    return true;
}

bool is_valid_ip(const char* ip) {
    // Simple validation - just check basic format
    if (ip == NULL) return false;
    
    int dots = 0;
    for (int i = 0; ip[i] != '\0'; i++) {
        if (ip[i] == '.') dots++;
        else if (!isdigit(ip[i])) return false;
    }
    
    return (dots == 3);
}

bool is_valid_port(int port) {
    return (port > 0 && port < 65536);
}

// ==================== HASH FUNCTION ====================

unsigned long hash_string(const char* str) {
    unsigned long hash = 5381;
    int c;
    
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
    }
    
    return hash;
}

// ==================== ERROR TO STRING ====================

const char* error_to_string(ErrorCode err) {
    switch (err) {
        case ERR_SUCCESS: return "Success";
        case ERR_FILE_NOT_FOUND: return "File not found";
        case ERR_ACCESS_DENIED: return "Access denied";
        case ERR_FILE_EXISTS: return "File already exists";
        case ERR_SENTENCE_LOCKED: return "Sentence is locked by another user";
        case ERR_INVALID_INDEX: return "Invalid index";
        case ERR_SS_UNAVAILABLE: return "Storage server unavailable";
        case ERR_INVALID_COMMAND: return "Invalid command";
        case ERR_PERMISSION_DENIED: return "Permission denied";
        case ERR_NOT_OWNER: return "You are not the owner";
        case ERR_CONNECTION_FAILED: return "Connection failed";
        case ERR_TIMEOUT: return "Operation timed out";
        case ERR_INVALID_SENTENCE: return "Invalid sentence index";
        case ERR_INVALID_WORD_INDEX: return "Invalid word index";
        case ERR_FILE_LOCKED: return "File is locked";
        default: return "Unknown error";
    }
}
