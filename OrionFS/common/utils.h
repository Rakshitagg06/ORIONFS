#ifndef UTILS_H
#define UTILS_H

#include "protocol.h"

// ==================== LOGGING ====================

// Log levels
typedef enum {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARNING,
    LOG_ERROR,
    LOG_CRITICAL
} LogLevel;

// Initialize logging system
void init_logger(const char* log_file_path, const char* component_name);

// Log a message with timestamp, IP, port, username, action, details, status
void log_message(LogLevel level, const char* ip, int port, 
                 const char* username, const char* action, 
                 const char* details, const char* status);

// Close logging system
void close_logger();

// ==================== STRING UTILITIES ====================

// Trim leading and trailing whitespace
char* trim_string(char* str);

// Split string by delimiter
int split_string(char* str, char delimiter, char** tokens, int max_tokens);

// Check if string ends with delimiter (. ! ?)
bool is_sentence_delimiter(char c);

// Parse a sentence into words
int parse_sentence_to_words(char* sentence, char** words, int max_words);

// Join words into a sentence
void join_words_to_sentence(char** words, int word_count, char* sentence, int max_len);

// ==================== TIME UTILITIES ====================

// Get current timestamp string (YYYY-MM-DD HH:MM:SS)
void get_timestamp_string(char* buffer, int buffer_size);

// Get current timestamp (time_t)
time_t get_current_timestamp();

// Convert time_t to string
void timestamp_to_string(time_t timestamp, char* buffer, int buffer_size);

// ==================== FILE UTILITIES ====================

// Check if file exists
bool file_exists(const char* filepath);

// Get file size in bytes
size_t get_file_size(const char* filepath);

// Count words in file
int count_words_in_file(const char* filepath);

// Count characters in file
int count_chars_in_file(const char* filepath);

// Read entire file content
char* read_file_content(const char* filepath);

// Write content to file
int write_file_content(const char* filepath, const char* content);

// Atomic file write (write to temp, then rename)
int atomic_write_file(const char* filepath, const char* content);

// ==================== PARSING UTILITIES ====================

// Parse command from user input
typedef struct {
    char cmd[32];
    char args[10][MAX_FILENAME_LEN];
    int arg_count;
    bool flag_a;  // -a flag (all files)
    bool flag_l;  // -l flag (list with details)
    bool flag_r;  // -R flag (read access)
    bool flag_w;  // -W flag (write access)
} Command;

Command parse_command(char* input);

// ==================== VALIDATION ====================

// Validate filename (no special chars, reasonable length)
bool is_valid_filename(const char* filename);

// Validate username
bool is_valid_username(const char* username);

// Validate IP address
bool is_valid_ip(const char* ip);

// Validate port number
bool is_valid_port(int port);

// ==================== HASH FUNCTION ====================

// Simple hash function for string (for hash map)
unsigned long hash_string(const char* str);

#endif // UTILS_H
