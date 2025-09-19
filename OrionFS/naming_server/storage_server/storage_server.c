#include "../common/protocol.h"
#include "../common/utils.h"
#include "../common/network.h"
#include <pthread.h>
#include <sys/stat.h>
#include <dirent.h>

char ss_id[64];
char storage_dir[512];
char nm_ip[MAX_IP_LEN];
int nm_port;
int ss_nm_port;
int ss_client_port;
int nm_socket_fd = -1;

// Undo storage: filename -> previous content
typedef struct {
    char filename[MAX_FILENAME_LEN];
    char* content;
} UndoStorage;

UndoStorage undo_storage[1000];
int undo_count = 0;
pthread_mutex_t undo_mutex = PTHREAD_MUTEX_INITIALIZER;

// Sentence locks: (filename, sentence_idx) -> username
typedef struct {
    char filename[MAX_FILENAME_LEN];
    int sentence_idx;
    char locked_by[MAX_USERNAME_LEN];
    bool is_locked;
    pthread_mutex_t mutex;
} SentenceLockEntry;

SentenceLockEntry sentence_locks[1000];
int lock_count = 0;
pthread_mutex_t lock_table_mutex = PTHREAD_MUTEX_INITIALIZER;

SentenceLockEntry* get_or_create_lock(const char* filename, int sentence_idx) {
    pthread_mutex_lock(&lock_table_mutex);
    
    // Find existing
    for (int i = 0; i < lock_count; i++) {
        if (strcmp(sentence_locks[i].filename, filename) == 0 &&
            sentence_locks[i].sentence_idx == sentence_idx) {
            pthread_mutex_unlock(&lock_table_mutex);
            return &sentence_locks[i];
        }
    }
    
    // Create new
    if (lock_count < 1000) {
        SentenceLockEntry* entry = &sentence_locks[lock_count++];
        strcpy(entry->filename, filename);
        entry->sentence_idx = sentence_idx;
        entry->is_locked = false;
        pthread_mutex_init(&entry->mutex, NULL);
        pthread_mutex_unlock(&lock_table_mutex);
        return entry;
    }
    
    pthread_mutex_unlock(&lock_table_mutex);
    return NULL;
}

bool try_acquire_lock(const char* filename, int sentence_idx, const char* username) {
    SentenceLockEntry* entry = get_or_create_lock(filename, sentence_idx);
    if (!entry) return false;
    
    pthread_mutex_lock(&entry->mutex);
    if (entry->is_locked) {
        pthread_mutex_unlock(&entry->mutex);
        return false;
    }
    
    entry->is_locked = true;
    strcpy(entry->locked_by, username);
    pthread_mutex_unlock(&entry->mutex);
    return true;
}

void release_lock(const char* filename, int sentence_idx) {
    SentenceLockEntry* entry = get_or_create_lock(filename, sentence_idx);
    if (!entry) return;
    
    pthread_mutex_lock(&entry->mutex);
    entry->is_locked = false;
    entry->locked_by[0] = '\0';
    pthread_mutex_unlock(&entry->mutex);
}

void save_undo(const char* filename, const char* content) {
    pthread_mutex_lock(&undo_mutex);
    
    // Find or create entry
    int idx = -1;
    for (int i = 0; i < undo_count; i++) {
        if (strcmp(undo_storage[i].filename, filename) == 0) {
            idx = i;
            break;
        }
    }
    
    if (idx == -1 && undo_count < 1000) {
        idx = undo_count++;
        strcpy(undo_storage[idx].filename, filename);
        undo_storage[idx].content = NULL;
    }
    
    if (idx >= 0) {
        if (undo_storage[idx].content) free(undo_storage[idx].content);
        undo_storage[idx].content = strdup(content);
    }
    
    pthread_mutex_unlock(&undo_mutex);
}

char* get_undo(const char* filename) {
    pthread_mutex_lock(&undo_mutex);
    
    for (int i = 0; i < undo_count; i++) {
        if (strcmp(undo_storage[i].filename, filename) == 0) {
            char* content = undo_storage[i].content;
            undo_storage[i].content = NULL;
            pthread_mutex_unlock(&undo_mutex);
            return content;
        }
    }
    
    pthread_mutex_unlock(&undo_mutex);
    return NULL;
}

void get_filepath(const char* filename, char* filepath) {
    snprintf(filepath, 512, "%s/%s", storage_dir, filename);
}

void handle_create_file(Message* msg, int socket) {
    Message response = {0};
    
    char filepath[512];
    get_filepath(msg->filename, filepath);
    
    if (file_exists(filepath)) {
        response.type = MSG_ERROR;
        response.error_code = ERR_FILE_EXISTS;
        strcpy(response.data, "File exists");
        send_message(socket, &response);
        return;
    }
    
    FILE* f = fopen(filepath, "w");
    if (!f) {
        response.type = MSG_ERROR;
        response.error_code = ERR_UNKNOWN;
        send_message(socket, &response);
        return;
    }
    fclose(f);
    
    response.type = MSG_ACK;
    response.error_code = ERR_SUCCESS;
    strcpy(response.data, "File created");
    send_message(socket, &response);
    
    log_message(LOG_INFO, "nm", 0, "system", "CREATE", msg->filename, "SUCCESS");
    printf("✅ Created file: %s\n", msg->filename);
}

void handle_delete_file(Message* msg, int socket) {
    Message response = {0};
    
    char filepath[512];
    get_filepath(msg->filename, filepath);
    
    if (unlink(filepath) == 0) {
        response.type = MSG_ACK;
        response.error_code = ERR_SUCCESS;
        strcpy(response.data, "File deleted");
        log_message(LOG_INFO, "nm", 0, "system", "DELETE", msg->filename, "SUCCESS");
    } else {
        response.type = MSG_ERROR;
        response.error_code = ERR_FILE_NOT_FOUND;
    }
    
    send_message(socket, &response);
}

void handle_read_file(Message* msg, int client_socket) {
    Message response = {0};
    
    char filepath[512];
    get_filepath(msg->filename, filepath);
    
    char* content = read_file_content(filepath);
    if (!content) {
        response.type = MSG_ERROR;
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "File not found");
        send_message(client_socket, &response);
        return;
    }
    
    response.type = MSG_ACK;
    strncpy(response.data, content, MAX_CONTENT_LEN - 1);
    send_message(client_socket, &response);
    
    free(content);
    log_message(LOG_INFO, "client", 0, msg->sender, "READ", msg->filename, "SUCCESS");
}

// Parse content into sentences (by . ! ?)
int parse_sentences(const char* content, char*** sentences_out) {
    if (!content || strlen(content) == 0) {
        *sentences_out = NULL;
        return 0;
    }
    
    char** sentences = malloc(MAX_SENTENCES * sizeof(char*));
    int count = 0;
    
    char* content_copy = strdup(content);
    char* start = content_copy;
    char* p = content_copy;
    
    while (*p) {
        if (is_sentence_delimiter(*p)) {
            // Found delimiter, extract sentence including delimiter
            int len = p - start + 1;
            sentences[count] = malloc(len + 1);
            strncpy(sentences[count], start, len);
            sentences[count][len] = '\0';
            count++;
            start = p + 1;
            
            // Skip spaces after delimiter
            while (*start && *start == ' ') start++;
            p = start;
        } else {
            p++;
        }
    }
    
    // Remaining text (no delimiter)
    if (start < p) {
        sentences[count] = strdup(start);
        count++;
    }
    
    free(content_copy);
    *sentences_out = sentences;
    return count;
}

void handle_write_file(Message* msg, int client_socket) {
    Message response = {0};
    
    char filepath[512];
    get_filepath(msg->filename, filepath);
    
    // Read current content
    char* current_content = read_file_content(filepath);
    if (!current_content) {
        current_content = strdup("");
    }
    
    int sentence_idx = msg->arg1;
    
    // Validate index (allow writing to sentence 0 even if file is empty)
    if (sentence_idx < 0 || sentence_idx > 100) {
        response.type = MSG_ERROR;
        response.error_code = ERR_INVALID_INDEX;
        strcpy(response.data, "Invalid sentence index");
        send_message(client_socket, &response);
        free(current_content);
        return;
    }
    
    // Try to acquire lock
    if (!try_acquire_lock(msg->filename, sentence_idx, msg->sender)) {
        response.type = MSG_ERROR;
        response.error_code = ERR_SENTENCE_LOCKED;
        strcpy(response.data, "Sentence is locked");
        send_message(client_socket, &response);
        free(current_content);
        return;
    }
    
    // Save for undo
    save_undo(msg->filename, current_content);
    
    // Send ready
    response.type = MSG_ACK;
    strcpy(response.data, "READY");
    send_message(client_socket, &response);
    
    // Collect words from client
    char** words = malloc(MAX_WORDS_PER_SENTENCE * sizeof(char*));
    for (int i = 0; i < MAX_WORDS_PER_SENTENCE; i++) {
        words[i] = NULL;
    }
    int max_word_idx = -1;
    
    // Interactive mode: receive word updates
    while (1) {
        Message update_msg;
        int received = recv_message(client_socket, &update_msg);
        
        if (received <= 0 || strcmp(update_msg.data, "ETIRW") == 0) {
            break;
        }
        
        // Parse: word_idx and content
        int word_idx = update_msg.arg1;
        char* word_content = update_msg.data;
        
        if (word_idx >= 0 && word_idx < MAX_WORDS_PER_SENTENCE) {
            if (words[word_idx]) {
                free(words[word_idx]);
            }
            words[word_idx] = strdup(word_content);
            if (word_idx > max_word_idx) {
                max_word_idx = word_idx;
            }
        }
    }
    
    // Rebuild sentence from words
    char new_content[MAX_CONTENT_LEN] = "";
    for (int i = 0; i <= max_word_idx; i++) {
        if (words[i]) {
            if (strlen(new_content) > 0) {
                strcat(new_content, " ");
            }
            strcat(new_content, words[i]);
        }
    }
    
    // Write to file
    atomic_write_file(filepath, new_content);
    
    // Release lock
    release_lock(msg->filename, sentence_idx);
    
    // Send ACK
    response.type = MSG_ACK;
    strcpy(response.data, "Write successful!");
    send_message(client_socket, &response);
    
    // Cleanup
    for (int i = 0; i < MAX_WORDS_PER_SENTENCE; i++) {
        if (words[i]) free(words[i]);
    }
    free(words);
    free(current_content);
    
    log_message(LOG_INFO, "client", 0, msg->sender, "WRITE", msg->filename, "SUCCESS");
}

void handle_undo_file(Message* msg, int client_socket) {
    Message response = {0};
    
    char* undo_content = get_undo(msg->filename);
    if (!undo_content) {
        response.type = MSG_ERROR;
        response.error_code = ERR_UNKNOWN;
        strcpy(response.data, "No undo history");
        send_message(client_socket, &response);
        return;
    }
    
    char filepath[512];
    get_filepath(msg->filename, filepath);
    atomic_write_file(filepath, undo_content);
    
    free(undo_content);
    
    response.type = MSG_ACK;
    strcpy(response.data, "Undo successful!");
    send_message(client_socket, &response);
    
    log_message(LOG_INFO, "client", 0, msg->sender, "UNDO", msg->filename, "SUCCESS");
}

void handle_stream_file(Message* msg, int client_socket) {
    char filepath[512];
    get_filepath(msg->filename, filepath);
    
    char* content = read_file_content(filepath);
    if (!content) {
        Message response = {0};
        response.type = MSG_ERROR;
        response.error_code = ERR_FILE_NOT_FOUND;
        send_message(client_socket, &response);
        return;
    }
    
    // Parse into words
    char** words = malloc(10000 * sizeof(char*));
    int word_count = 0;
    
    char* token = strtok(content, " \t\n");
    while (token && word_count < 10000) {
        words[word_count++] = strdup(token);
        token = strtok(NULL, " \t\n");
    }
    
    // Stream word by word
    for (int i = 0; i < word_count; i++) {
        Message word_msg = {0};
        word_msg.type = MSG_ACK;
        strcpy(word_msg.data, words[i]);
        send_message(client_socket, &word_msg);
        
        usleep(100000); // 0.1 second delay
        free(words[i]);
    }
    
    // Send END
    Message end_msg = {0};
    end_msg.type = MSG_ACK;
    strcpy(end_msg.data, "END_OF_FILE");
    send_message(client_socket, &end_msg);
    
    free(words);
    free(content);
    
    log_message(LOG_INFO, "client", 0, msg->sender, "STREAM", msg->filename, "SUCCESS");
}

void* handle_client_connection(void* arg) {
    int client_socket = *(int*)arg;
    free(arg);
    
    Message msg;
    int received = recv_message(client_socket, &msg);
    
    if (received <= 0) {
        close_socket(client_socket);
        return NULL;
    }
    
    switch (msg.type) {
        case MSG_READ_REQUEST:
            handle_read_file(&msg, client_socket);
            break;
        case MSG_WRITE_REQUEST:
            handle_write_file(&msg, client_socket);
            break;
        case MSG_STREAM_REQUEST:
            handle_stream_file(&msg, client_socket);
            break;
        case MSG_UNDO_FILE:
            handle_undo_file(&msg, client_socket);
            break;
        default:
            break;
    }
    
    close_socket(client_socket);
    return NULL;
}

void register_with_nm() {
    printf("📡 Registering with Naming Server...\n");
    
    nm_socket_fd = connect_to_server(nm_ip, nm_port);
    if (nm_socket_fd < 0) {
        printf("❌ Failed to connect to NM\n");
        exit(1);
    }
    
    Message reg_msg = {0};
    reg_msg.type = MSG_SS_REGISTER;
    strcpy(reg_msg.sender, ss_id);
    reg_msg.arg1 = ss_nm_port;
    reg_msg.arg2 = ss_client_port;
    
    send_message(nm_socket_fd, &reg_msg);
    
    Message response;
    recv_message(nm_socket_fd, &response);
    
    printf("✅ %s\n", response.data);
}

void* nm_communication_thread(void* arg) {
    while (1) {
        Message msg;
        int received = recv_message(nm_socket_fd, &msg);
        
        if (received <= 0) {
            printf("❌ Lost connection to NM\n");
            break;
        }
        
        switch (msg.type) {
            case MSG_CREATE_FILE_SS:
                handle_create_file(&msg, nm_socket_fd);
                break;
            case MSG_DELETE_FILE_SS:
                handle_delete_file(&msg, nm_socket_fd);
                break;
            default:
                break;
        }
    }
    
    return NULL;
}

int main(int argc, char* argv[]) {
    if (argc < 6) {
        printf("Usage: %s <ss_id> <nm_port> <nm_ip> <ss_nm_port> <ss_client_port>\n", argv[0]);
        return 1;
    }
    
    strcpy(ss_id, argv[1]);
    nm_port = atoi(argv[2]);
    strcpy(nm_ip, argv[3]);
    ss_nm_port = atoi(argv[4]);
    ss_client_port = atoi(argv[5]);
    
    printf("🚀 Starting Storage Server %s...\n", ss_id);
    
    // Create storage directory
    sprintf(storage_dir, "data/ss_storage/%s", ss_id);
    mkdir("data", 0777);
    mkdir("data/ss_storage", 0777);
    mkdir(storage_dir, 0777);
    
    // Initialize logger
    char log_file[256];
    sprintf(log_file, "logs/%s_logs.txt", ss_id);
    char component[16];
    strcpy(component, ss_id);
    init_logger(log_file, component);
    
    // Register with NM
    register_with_nm();
    
    // Start NM communication thread
    pthread_t nm_thread;
    pthread_create(&nm_thread, NULL, nm_communication_thread, NULL);
    pthread_detach(nm_thread);
    
    // Start client server
    int server_socket = start_server(ss_client_port, 50);
    if (server_socket < 0) {
        printf("❌ Failed to start client server\n");
        return 1;
    }
    
    printf("✅ Storage Server %d ready on port %d\n\n", ss_id, ss_client_port);
    
    while (1) {
        char client_ip[MAX_IP_LEN];
        int client_port;
        
        int client_socket = accept_connection(server_socket, client_ip, &client_port);
        if (client_socket < 0) continue;
        
        pthread_t thread;
        int* socket_ptr = malloc(sizeof(int));
        *socket_ptr = client_socket;
        pthread_create(&thread, NULL, handle_client_connection, socket_ptr);
        pthread_detach(thread);
    }
    
    close_logger();
    return 0;
}
