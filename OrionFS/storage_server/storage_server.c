#include "../common/protocol.h"
#include "../common/utils.h"
#include "../common/network.h"
#include <pthread.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/types.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

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

// File-level commit locks to serialize final merge/write per file
typedef struct {
    char filename[MAX_FILENAME_LEN];
    pthread_mutex_t mutex;
} FileCommitLock;

static FileCommitLock file_commit_locks[1000];
static int file_commit_lock_count = 0;
static pthread_mutex_t file_commit_table_mutex = PTHREAD_MUTEX_INITIALIZER;

static FileCommitLock* get_or_create_file_commit_lock(const char* filename) {
    pthread_mutex_lock(&file_commit_table_mutex);
    for (int i=0; i<file_commit_lock_count; ++i) {
        if (strcmp(file_commit_locks[i].filename, filename) == 0) {
            pthread_mutex_unlock(&file_commit_table_mutex);
            return &file_commit_locks[i];
        }
    }
    if (file_commit_lock_count < 1000) {
        FileCommitLock* e = &file_commit_locks[file_commit_lock_count++];
        strncpy(e->filename, filename, MAX_FILENAME_LEN-1);
        e->filename[MAX_FILENAME_LEN-1] = '\0';
        pthread_mutex_init(&e->mutex, NULL);
        pthread_mutex_unlock(&file_commit_table_mutex);
        return e;
    }
    pthread_mutex_unlock(&file_commit_table_mutex);
    return NULL;
}

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

// Try to locate an existing file either in root or any first-level subfolder under storage_dir.
// Returns true and writes absolute path into out if found.
static bool resolve_existing_file_path(const char* filename, char* out, size_t out_sz) {
    // 1) Root
    char candidate[600];
    snprintf(candidate, sizeof(candidate), "%s/%s", storage_dir, filename);
    if (file_exists(candidate)) { strncpy(out, candidate, out_sz - 1); out[out_sz - 1] = '\0'; return true; }
    // 2) First-level folders
    DIR* dir = opendir(storage_dir);
    if (!dir) return false;
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        // skip non-directories like regular data files and known meta folders
        char subpath[600];
        snprintf(subpath, sizeof(subpath), "%s/%s", storage_dir, ent->d_name);
        struct stat st; if (stat(subpath, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        // check candidate inside this folder
        char inner[700];
        snprintf(inner, sizeof(inner), "%s/%s", subpath, filename);
        if (file_exists(inner)) { closedir(dir); strncpy(out, inner, out_sz - 1); out[out_sz - 1] = '\0'; return true; }
    }
    closedir(dir);
    return false;
}

// Build checkpoint directory base for a file: place under the file's containing folder if any,
// otherwise under storage_dir/checkpoints/<filename>/
static void build_checkpoint_dir_for_file(const char* filename, char* out_dir, size_t out_sz) {
    char fpath[700];
    if (resolve_existing_file_path(filename, fpath, sizeof(fpath))) {
        // parent directory of the file
        char parent[700];
        strncpy(parent, fpath, sizeof(parent)-1); parent[sizeof(parent)-1] = '\0';
        char *slash = strrchr(parent, '/');
        if (slash) { *slash = '\0'; }
        // checkpoints directory beside the file's folder
        snprintf(out_dir, out_sz, "%s/checkpoints/%s", parent, filename);
    } else {
        // fallback to root checkpoints
        snprintf(out_dir, out_sz, "%s/checkpoints/%s", storage_dir, filename);
    }
}

// Recursively create directories in a path (like mkdir -p). Returns 0 on success, -1 on error.
static int mkdir_p(const char *path, mode_t mode) {
    char tmp[1024];
    size_t len = strnlen(path, sizeof(tmp));
    if (len == 0 || len >= sizeof(tmp)) return -1;
    strncpy(tmp, path, sizeof(tmp)-1); tmp[sizeof(tmp)-1] = '\0';
    if (tmp[len-1] == '/') tmp[len-1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1; *p = '/'; }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
    return 0;
}

// Ensure a backup copy under data/ss_storage/backup mirrors the latest content
// of the given filename. This mirrors any subfolder structure inside backup/ as well.
static void update_backup_for_file(const char *filename, const char *content) {
    const char *backup_root = "data/ss_storage/backup";
    // Ensure base dirs exist
    mkdir("data", 0777);
    mkdir("data/ss_storage", 0777);
    mkdir(backup_root, 0777);

    // Build backup path (mirror subfolders if present in filename)
    char backup_path[1024];
    snprintf(backup_path, sizeof(backup_path), "%s/%s", backup_root, filename);

    // Ensure parent directories for the backup file exist
    char dirbuf[1024];
    strncpy(dirbuf, backup_path, sizeof(dirbuf)-1);
    dirbuf[sizeof(dirbuf)-1] = '\0';
    char *slash = strrchr(dirbuf, '/');
    if (slash) {
        *slash = '\0';
        mkdir_p(dirbuf, 0777);
    }

    // Write content atomically to the backup copy
    atomic_write_file(backup_path, content ? content : "");
}

// Compare two sentence strings after trimming leading/trailing whitespace.
// Returns 1 if equal, 0 otherwise.
static int sentences_equal_trimmed(const char *a, const char *b) {
    if (!a || !b) return 0;
    char *ca = strdup(a);
    char *cb = strdup(b);
    if (!ca || !cb) { if (ca) free(ca); if (cb) free(cb); return 0; }
    char *ta = trim_string(ca);
    char *tb = trim_string(cb);
    int eq = (strcmp(ta, tb) == 0);
    free(ca);
    free(cb);
    return eq;
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

    // Also create/update a backup copy (initial empty snapshot) under data/ss_storage/backup
    // Backup is never deleted on client DELETE to allow recovery.
    update_backup_for_file(msg->filename, "");
    
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
    
    char filepath[700];
    if (!resolve_existing_file_path(msg->filename, filepath, sizeof(filepath))) {
        // fallback to root path to read (will fail below and report not found)
        get_filepath(msg->filename, filepath);
    }
    
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
    const char *action = (msg->type == MSG_EXEC_READ_REQUEST ? "EXEC" : "READ");
    log_message(LOG_INFO, "client", 0, msg->sender, action, msg->filename, "SUCCESS");
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
    
    // Remaining text (no delimiter) – keep as a sentence segment to allow editing index 0,
    // but appending a new sentence after a non-terminated last segment will be rejected later.
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
    
    char filepath[700];
    if (!resolve_existing_file_path(msg->filename, filepath, sizeof(filepath))) {
        // Must exist for write (we don't support implicit create in WRITE)
        get_filepath(msg->filename, filepath);
    }
    
    // Read current content
    char* current_content = read_file_content(filepath);
    if (!current_content) {
        current_content = strdup("");
    }
    
    int sentence_idx = msg->arg1;
    
    // Parse existing content into sentences
    char** sentences = NULL;
    int sentence_count = parse_sentences(current_content, &sentences);
    // Track whether this session started as an append beyond the last sentence
    bool initial_append = (sentence_idx == sentence_count);
    
    // Validate index with delimiter rule:
    // - Can write to existing sentences [0 .. sentence_count-1]
    // - Can append at index == sentence_count only if the last existing sentence ended with a delimiter
    //   (i.e., content's last non-space character is . ! or ?)
    bool last_has_delim = false;
    if (sentence_count > 0) {
        // Inspect original content's last non-space char
        const char* endp = current_content + strlen(current_content);
        while (endp > current_content && (*(endp-1) == ' ' || *(endp-1) == '\n' || *(endp-1) == '\r' || *(endp-1) == '\t')) endp--;
        if (endp > current_content) {
            last_has_delim = is_sentence_delimiter(*(endp-1));
        }
    }
    if (sentence_idx < 0 || sentence_idx > sentence_count || (sentence_idx == sentence_count && sentence_count > 0 && !last_has_delim)) {
        response.type = MSG_ERROR;
        response.error_code = ERR_INVALID_INDEX;
        strcpy(response.data, "ERROR: Sentence index out of range.");
        send_message(client_socket, &response);
        free(current_content);
        if (sentences) {
            for (int i = 0; i < sentence_count; i++) free(sentences[i]);
            free(sentences);
        }
        return;
    }
    
    // Try to acquire lock
    if (!try_acquire_lock(msg->filename, sentence_idx, msg->sender)) {
        response.type = MSG_ERROR;
        response.error_code = ERR_SENTENCE_LOCKED;
        strcpy(response.data, "Sentence is locked");
        send_message(client_socket, &response);
        free(current_content);
        if (sentences) {
            for (int i = 0; i < sentence_count; i++) free(sentences[i]);
            free(sentences);
        }
        return;
    }
    
    // Save for undo
    save_undo(msg->filename, current_content);
    
    // Send ready
    response.type = MSG_ACK;
    strcpy(response.data, "READY");
    send_message(client_socket, &response);
    
    // Working buffers owned by this session
    char target_sentence[MAX_CONTENT_LEN] = "";
    if (sentence_idx < sentence_count) {
        strncpy(target_sentence, sentences[sentence_idx], MAX_CONTENT_LEN - 1);
    }
    
    char** sentence_words = malloc(MAX_WORDS_PER_SENTENCE * sizeof(char*));
    int word_count = 0;
    if (strlen(target_sentence) > 0) {
        char temp[MAX_CONTENT_LEN];
        strcpy(temp, target_sentence);
        char* token = strtok(temp, " ");
        while (token && word_count < MAX_WORDS_PER_SENTENCE) {
            sentence_words[word_count++] = strdup(token);
            token = strtok(NULL, " ");
        }
    }
    // Capture original and previous sentence snapshots for remapping at commit
    char original_sentence_snapshot[MAX_CONTENT_LEN] = "";
    if (sentence_idx < sentence_count && sentences[sentence_idx]) {
        strncpy(original_sentence_snapshot, sentences[sentence_idx], sizeof(original_sentence_snapshot)-1);
        original_sentence_snapshot[sizeof(original_sentence_snapshot)-1] = '\0';
    }
    char prev_sentence_snapshot[MAX_CONTENT_LEN] = "";
    if (sentence_idx > 0 && (sentence_idx-1) < sentence_count && sentences[sentence_idx-1]) {
        strncpy(prev_sentence_snapshot, sentences[sentence_idx-1], sizeof(prev_sentence_snapshot)-1);
        prev_sentence_snapshot[sizeof(prev_sentence_snapshot)-1] = '\0';
    }

    bool committed = false; // track if ETIRW was received
    
    // Interactive mode: receive word insertions
    while (1) {
        Message update_msg;
        int received = recv_message(client_socket, &update_msg);
        
        if (received <= 0) {
            // Client disconnected abruptly: do NOT commit, restore undo snapshot
            // Release lock and cleanup, then exit
            break;
        }
        
        if (strcmp(update_msg.data, "ETIRW") == 0) {
            committed = true;
            break;
        }
        
        // Parse: word_idx and content (client index is 1-based; enforce strictly)
        int word_idx = update_msg.arg1;
        if (word_idx <= 0) {
            Message err = {0};
            err.type = MSG_ERROR;
            err.error_code = ERR_INVALID_WORD_INDEX;
            strcpy(err.data, "ERROR: Invalid word index. Use 1-based indexing.");
            send_message(client_socket, &err);
            continue;
        }
        word_idx -= 1; // convert to 0-based
        char* word_content = update_msg.data;
        
        // Validate word index (can insert at any position from 0..word_count inclusive [append])
        if (word_idx < 0 || word_idx > word_count || word_count >= MAX_WORDS_PER_SENTENCE) {
            Message err = {0};
            err.type = MSG_ERROR;
            err.error_code = ERR_INVALID_WORD_INDEX;
            strcpy(err.data, "ERROR: Invalid word index. Use 1..current_length+1.");
            send_message(client_socket, &err);
            continue; // keep session alive, just reject this insertion
        }
        
        // Insert word at position (shift if inserting before end)
    bool added_new_token = true;
    if (word_idx < word_count) {
            // Edge case: if inserting immediately after a word that ends with a sentence delimiter,
            // merge the new content into the previous word before its delimiter (e.g., "test?" + "hhs" -> "testhhs?").
            if (word_idx > 0) {
                char *prev = sentence_words[word_idx - 1];
                size_t plen = prev ? strlen(prev) : 0;
                if (plen > 0 && is_sentence_delimiter(prev[plen - 1])) {
                    char punct = prev[plen - 1];
                    // build merged token = prev_without_punct + word_content + punct
                    char *merged = (char*)malloc(plen + strlen(word_content) + 1);
                    if (!merged) {
                        // Fallback to regular insert on allocation failure
                        for (int i = word_count; i > word_idx; i--) { sentence_words[i] = sentence_words[i - 1]; }
                        sentence_words[word_idx] = strdup(word_content);
                    } else {
                        // copy prev without last char
                        memcpy(merged, prev, plen - 1);
                        merged[plen - 1] = '\0';
                        // append content
                        strncat(merged, word_content, (plen + strlen(word_content)));
                        // append punctuation
                        size_t ml = strlen(merged);
                        merged[ml] = punct; merged[ml+1] = '\0';
                        // replace previous token
                        free(sentence_words[word_idx - 1]);
                        sentence_words[word_idx - 1] = merged;
                        // Do not shift/insert a new word in this merge path; keep word_count same
                        added_new_token = false;
                        goto AFTER_INSERTION;
                    }
                } else {
                    // normal insertion if no delimiter on previous
                    for (int i = word_count; i > word_idx; i--) {
                        sentence_words[i] = sentence_words[i - 1];
                    }
                    sentence_words[word_idx] = strdup(word_content);
                }
            } else {
                // normal insertion at start
                for (int i = word_count; i > word_idx; i--) {
                    sentence_words[i] = sentence_words[i - 1];
                }
                sentence_words[word_idx] = strdup(word_content);
            }
        } else { // word_idx == word_count -> append
            // Append with punctuation-aware behavior:
            // - If the last token is a standalone delimiter (".", "!", "?"), insert before it.
            // - Else if the last token ends with a delimiter, merge new content before that delimiter (e.g., "test?" + "jj" -> "testjj?").
            // - Else, simple append.
            if (word_count > 0) {
                char *last = sentence_words[word_count - 1];
                size_t llen = last ? strlen(last) : 0;
                size_t wlen = strlen(word_content);
                if (llen == 1 && is_sentence_delimiter(last[0]) && (wlen == 0 || !is_sentence_delimiter(word_content[wlen - 1]))) {
                    // Insert new token before the standalone punctuation token
                    for (int i = word_count; i > word_idx; i--) {
                        sentence_words[i] = sentence_words[i - 1];
                    }
                    sentence_words[word_idx] = strdup(word_content);
                } else if (llen > 0 && is_sentence_delimiter(last[llen - 1]) && (wlen == 0 || !is_sentence_delimiter(word_content[wlen - 1]))) {
                    // Merge into last token before its trailing punctuation
                    char punct = last[llen - 1];
                    char *merged = (char*)malloc(llen /*including punct*/ + wlen + 1); // we'll reuse last's space; ensure enough
                    if (!merged) {
                        // fallback to append
                        sentence_words[word_count] = strdup(word_content);
                    } else {
                        // copy last without punctuation
                        memcpy(merged, last, llen - 1);
                        merged[llen - 1] = '\0';
                        // append new content
                        strncat(merged, word_content, wlen);
                        // append punctuation
                        size_t ml = strlen(merged);
                        merged[ml] = punct; merged[ml+1] = '\0';
                        free(sentence_words[word_count - 1]);
                        sentence_words[word_count - 1] = merged;
                        added_new_token = false; // no new token added
                    }
                } else {
                    // simple append
                    sentence_words[word_count] = strdup(word_content);
                }
            } else {
                sentence_words[word_count] = strdup(word_content);
            }
        }
AFTER_INSERTION:
        if (added_new_token) {
            word_count++;
        }
    }
    
    if (committed) {
        // Rebuild the modified sentence from words with punctuation compaction:
        // If a token is a standalone delimiter (".", "!", "?") and not first, attach directly
        // to previous token without a space (so "word" + "." -> "word.").
        // Otherwise insert space between tokens.
        char new_sentence[MAX_CONTENT_LEN] = "";
        for (int i = 0; i < word_count; i++) {
            const char *tok = sentence_words[i];
            size_t tlen = tok ? strlen(tok) : 0;
            bool is_single_punct = (tlen == 1 && is_sentence_delimiter(tok[0]));
            if (i == 0) {
                strcat(new_sentence, tok);
            } else if (is_single_punct) {
                // attach directly
                strcat(new_sentence, tok);
            } else {
                strcat(new_sentence, " ");
                strcat(new_sentence, tok);
            }
        }
        
        // Split the modified sentence into multiple sentences if delimiters exist
        char** split_arr = NULL;
        int split_cnt = parse_sentences(new_sentence, &split_arr);
        // Merge-commit with latest file state under per-file commit lock
        FileCommitLock* fcl = get_or_create_file_commit_lock(msg->filename);
        if (fcl) pthread_mutex_lock(&fcl->mutex);

        // Re-read latest content to avoid overwriting other concurrent sentence updates
        char* latest_content = read_file_content(filepath);
        if (!latest_content) latest_content = strdup("");
        char** latest_sentences = NULL; int latest_count = parse_sentences(latest_content, &latest_sentences);

        // Re-validate append rule on latest
        bool latest_last_has_delim = false;
        if (latest_count > 0) {
            const char* ep = latest_content + strlen(latest_content);
            while (ep > latest_content && (*(ep-1) == ' ' || *(ep-1) == '\n' || *(ep-1) == '\r' || *(ep-1) == '\t')) ep--;
            if (ep > latest_content) latest_last_has_delim = is_sentence_delimiter(*(ep-1));
        }

    // Determine the correct target index in the latest snapshot.
        // If this session started as an append, target is latest_count (subject to delimiter rule).
        // Otherwise, try to map the original sentence text to its current index to avoid overwriting
        // another user's intervening insertions that shifted indices.
        int target_index = sentence_idx;
        if (initial_append) {
            // Validate append rule on latest
            if (latest_count > 0 && !latest_last_has_delim) {
                if (fcl) pthread_mutex_unlock(&fcl->mutex);
                if (split_arr) { for (int k=0;k<split_cnt;++k) free(split_arr[k]); free(split_arr);}            
                free(latest_content);
                if (latest_sentences) { for (int i=0;i<latest_count;++i) free(latest_sentences[i]); free(latest_sentences);}            
                response.type = MSG_ERROR; response.error_code = ERR_INVALID_INDEX; strcpy(response.data, "ERROR: File changed; retry write.");
                send_message(client_socket, &response);
                committed = false;
                goto COMMIT_CLEANUP_SKIP;
            }
            target_index = latest_count; // append to end
        } else {
            // Non-append: remap based on original sentence text if needed
            if (sentence_idx < 0 || sentence_idx > latest_count) {
                if (fcl) pthread_mutex_unlock(&fcl->mutex);
                if (split_arr) { for (int k=0;k<split_cnt;++k) free(split_arr[k]); free(split_arr);}            
                free(latest_content);
                if (latest_sentences) { for (int i=0;i<latest_count;++i) free(latest_sentences[i]); free(latest_sentences);}            
                response.type = MSG_ERROR; response.error_code = ERR_INVALID_INDEX; strcpy(response.data, "ERROR: File changed; retry write.");
                send_message(client_socket, &response);
                committed = false;
                goto COMMIT_CLEANUP_SKIP;
            }
            // Try exact content match first using original snapshot
            if (original_sentence_snapshot[0] != '\0') {
                if (!(sentence_idx < latest_count && sentences_equal_trimmed(latest_sentences[sentence_idx], original_sentence_snapshot))) {
                    bool found=false;
                    for (int j=0;j<latest_count;++j) {
                        if (sentences_equal_trimmed(latest_sentences[j], original_sentence_snapshot)) { target_index = j; found=true; break; }
                    }
                    if (!found) {
                        // Try anchoring after previous sentence snapshot
                        if (prev_sentence_snapshot[0] != '\0') {
                            int anchor=-1;
                            for (int j=0;j<latest_count;++j) {
                                if (sentences_equal_trimmed(latest_sentences[j], prev_sentence_snapshot)) { anchor=j; break; }
                            }
                            if (anchor >= 0 && (anchor+1) < latest_count) {
                                target_index = anchor + 1;
                            } else {
                                if (fcl) pthread_mutex_unlock(&fcl->mutex);
                                if (split_arr) { for (int k=0;k<split_cnt;++k) free(split_arr[k]); free(split_arr);}            
                                free(latest_content);
                                if (latest_sentences) { for (int i=0;i<latest_count;++i) free(latest_sentences[i]); free(latest_sentences);}            
                                response.type = MSG_ERROR; response.error_code = ERR_INVALID_INDEX; strcpy(response.data, "ERROR: File changed; retry write.");
                                send_message(client_socket, &response);
                                committed = false;
                                goto COMMIT_CLEANUP_SKIP;
                            }
                        } else {
                            // No anchor; fall back conservatively
                            if (sentence_idx < latest_count) {
                                target_index = sentence_idx;
                            } else {
                                if (fcl) pthread_mutex_unlock(&fcl->mutex);
                                if (split_arr) { for (int k=0;k<split_cnt;++k) free(split_arr[k]); free(split_arr);}            
                                free(latest_content);
                                if (latest_sentences) { for (int i=0;i<latest_count;++i) free(latest_sentences[i]); free(latest_sentences);}            
                                response.type = MSG_ERROR; response.error_code = ERR_INVALID_INDEX; strcpy(response.data, "ERROR: File changed; retry write.");
                                send_message(client_socket, &response);
                                committed = false;
                                goto COMMIT_CLEANUP_SKIP;
                            }
                        }
                    }
                }
            } else if (prev_sentence_snapshot[0] != '\0') {
                // No original content (e.g., empty), use anchor-only strategy
                int anchor=-1;
                for (int j=0;j<latest_count;++j) {
                    if (sentences_equal_trimmed(latest_sentences[j], prev_sentence_snapshot)) { anchor=j; break; }
                }
                if (anchor >= 0 && (anchor+1) < latest_count) {
                    target_index = anchor + 1;
                } else {
                    if (fcl) pthread_mutex_unlock(&fcl->mutex);
                    if (split_arr) { for (int k=0;k<split_cnt;++k) free(split_arr[k]); free(split_arr);}            
                    free(latest_content);
                    if (latest_sentences) { for (int i=0;i<latest_count;++i) free(latest_sentences[i]); free(latest_sentences);}            
                    response.type = MSG_ERROR; response.error_code = ERR_INVALID_INDEX; strcpy(response.data, "ERROR: File changed; retry write.");
                    send_message(client_socket, &response);
                    committed = false;
                    goto COMMIT_CLEANUP_SKIP;
                }
            } else {
                // No snapshots; fall back or abort if out of range
                if (sentence_idx >= latest_count) {
                    if (fcl) pthread_mutex_unlock(&fcl->mutex);
                    if (split_arr) { for (int k=0;k<split_cnt;++k) free(split_arr[k]); free(split_arr);}            
                    free(latest_content);
                    if (latest_sentences) { for (int i=0;i<latest_count;++i) free(latest_sentences[i]); free(latest_sentences);}            
                    response.type = MSG_ERROR; response.error_code = ERR_INVALID_INDEX; strcpy(response.data, "ERROR: File changed; retry write.");
                    send_message(client_socket, &response);
                    committed = false;
                    goto COMMIT_CLEANUP_SKIP;
                }
                target_index = sentence_idx;
            }
        }

        if (split_cnt <= 1) {
            if (target_index < latest_count) {
                free(latest_sentences[target_index]);
                latest_sentences[target_index] = strdup(new_sentence);
            } else {
                latest_sentences = realloc(latest_sentences, (latest_count + 1) * sizeof(char*));
                latest_sentences[latest_count] = strdup(new_sentence);
                latest_count++;
            }
        } else {
            if (target_index < latest_count) {
                free(latest_sentences[target_index]);
                latest_sentences[target_index] = strdup(split_arr[0]);
                int extra = split_cnt - 1;
                latest_sentences = realloc(latest_sentences, (latest_count + extra) * sizeof(char*));
                for (int i = latest_count - 1; i > target_index; --i) {
                    latest_sentences[i + extra] = latest_sentences[i];
                }
                for (int k = 1; k < split_cnt; ++k) {
                    latest_sentences[target_index + k] = strdup(split_arr[k]);
                }
                latest_count += extra;
            } else {
                latest_sentences = realloc(latest_sentences, (latest_count + split_cnt) * sizeof(char*));
                for (int k = 0; k < split_cnt; ++k) {
                    latest_sentences[latest_count + k] = strdup(split_arr[k]);
                }
                latest_count += split_cnt;
            }
        }

        // Free split_arr
        if (split_arr) { for (int k=0;k<split_cnt;++k) free(split_arr[k]); free(split_arr); }

        // Rebuild merged final content
        char final_content[MAX_CONTENT_LEN * 10] = "";
        for (int i = 0; i < latest_count; i++) {
            if (i > 0 && strlen(final_content) > 0) strcat(final_content, " ");
            strcat(final_content, latest_sentences[i]);
        }
    atomic_write_file(filepath, final_content);
    // Also mirror the write into the backup copy
    update_backup_for_file(msg->filename, final_content);

        // Cleanup latest arrays
        for (int i=0;i<latest_count;++i) free(latest_sentences[i]);
        free(latest_sentences);
        free(latest_content);

        if (fcl) pthread_mutex_unlock(&fcl->mutex);
        
        // Update metadata in NM
        int word_count_total = 0;
        int char_count_total = strlen(final_content);
        char* temp = strdup(final_content);
        char* token = strtok(temp, " \t\n");
        while (token) {
            word_count_total++;
            token = strtok(NULL, " \t\n");
        }
        free(temp);
        
    Message meta_msg = (Message){0};
    meta_msg.type = MSG_UPDATE_METADATA;
    strcpy(meta_msg.filename, msg->filename);
    meta_msg.arg1 = word_count_total;
    meta_msg.arg2 = char_count_total;
    // Send metadata update (best-effort). Do not block client waiting for ACK.
    send_message(nm_socket_fd, &meta_msg);
    // Attempt non-blocking/short-timeout ACK read to drain socket without stalling
    set_socket_timeout(nm_socket_fd, 1);
    Message meta_ack; recv_message(nm_socket_fd, &meta_ack);
    set_socket_timeout(nm_socket_fd, 0);
    // Immediate ACK to client
    response.type = MSG_ACK;
    strcpy(response.data, "Write Successful!");
    send_message(client_socket, &response);
    } else {
        // Aborted: restore previous snapshot and do not modify file
        // (undo_storage already has pre-image; nothing to write)
    }

    // Release lock and cleanup
COMMIT_CLEANUP_SKIP:
    release_lock(msg->filename, sentence_idx);

    for (int i = 0; i < word_count; i++) {
        if (sentence_words[i]) free(sentence_words[i]);
    }
    free(sentence_words);
    
    for (int i = 0; i < sentence_count; i++) {
        free(sentences[i]);
    }
    free(sentences);
    free(current_content);

    if (committed) {
        log_message(LOG_INFO, "client", 0, msg->sender, "WRITE", msg->filename, "SUCCESS");
    } else {
        log_message(LOG_WARNING, "client", 0, msg->sender, "WRITE", msg->filename, "ABORTED");
    }
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
    
    char filepath[700];
    if (!resolve_existing_file_path(msg->filename, filepath, sizeof(filepath))) {
        get_filepath(msg->filename, filepath);
    }
    atomic_write_file(filepath, undo_content);
    // Mirror undo to backup as well
    update_backup_for_file(msg->filename, undo_content);
    
    free(undo_content);
    
    response.type = MSG_ACK;
    strcpy(response.data, "Undo Successful!");
    send_message(client_socket, &response);
    
    log_message(LOG_INFO, "client", 0, msg->sender, "UNDO", msg->filename, "SUCCESS");
}

void handle_stream_file(Message* msg, int client_socket) {
    char filepath[700];
    if (!resolve_existing_file_path(msg->filename, filepath, sizeof(filepath))) {
        get_filepath(msg->filename, filepath);
    }
    
    char* content = read_file_content(filepath);
    if (!content) {
        Message response = {0};
        response.type = MSG_ERROR;
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "ERROR: File not found.");
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
        case MSG_EXEC_READ_REQUEST:
            handle_read_file(&msg, client_socket);
            break;
        case MSG_UNDO_FILE:
            handle_undo_file(&msg, client_socket);
            break;
        case MSG_CP_CREATE:
        case MSG_CP_VIEW:
        case MSG_CP_REVERT:
        case MSG_CP_LIST:
            // Pass through to checkpoint handler block below
            break;
        default:
            break;
    }
    // For checkpoint operations, re-open file content or perform snapshot actions
    if (msg.type == MSG_CP_CREATE || msg.type == MSG_CP_VIEW || msg.type == MSG_CP_REVERT || msg.type == MSG_CP_LIST) {
        // Dispatch
        if (msg.type == MSG_CP_CREATE) {
            // Create snapshot next to the file's folder in a checkpoints subdir
            char filepath[700];
            if (!resolve_existing_file_path(msg.filename, filepath, sizeof(filepath))) {
                get_filepath(msg.filename, filepath);
            }
            char* content = read_file_content(filepath);
            if (!content) {
                Message r={0}; r.type=MSG_ERROR; r.error_code=ERR_FILE_NOT_FOUND; strcpy(r.data,"ERROR: File not found."); send_message(client_socket,&r);
            } else {
                // <parent>/checkpoints/<filename>/tag
                char cpdir[800]; build_checkpoint_dir_for_file(msg.filename, cpdir, sizeof(cpdir));
                mkdir_p(cpdir, 0777);
                char cpsnap[900]; snprintf(cpsnap,sizeof(cpsnap),"%s/%s.chk", cpdir, msg.data);
                atomic_write_file(cpsnap, content);
                free(content);
                Message r={0}; r.type=MSG_ACK; strcpy(r.data,"Checkpoint created successfully!"); send_message(client_socket,&r);
            }
        } else if (msg.type == MSG_CP_VIEW) {
            char cpdir[800]; build_checkpoint_dir_for_file(msg.filename, cpdir, sizeof(cpdir));
            char cpsnap[900]; snprintf(cpsnap,sizeof(cpsnap),"%s/%s.chk", cpdir, msg.data);
            char* snap = read_file_content(cpsnap);
            if (!snap) { Message r={0}; r.type=MSG_ERROR; r.error_code=ERR_FILE_NOT_FOUND; strcpy(r.data,"ERROR: Checkpoint not found."); send_message(client_socket,&r); }
            else { Message r={0}; r.type=MSG_ACK; strncpy(r.data,snap,MAX_CONTENT_LEN-1); send_message(client_socket,&r); free(snap);}        
        } else if (msg.type == MSG_CP_LIST) {
            char cpdir[800]; build_checkpoint_dir_for_file(msg.filename, cpdir, sizeof(cpdir));
            DIR* dir = opendir(cpdir);
            Message r={0}; r.type=MSG_ACK;
            if (dir) {
                struct dirent* ent; while ((ent=readdir(dir))) { if (ent->d_name[0]=='.') continue; // skip hidden
                    const char* d=ent->d_name; size_t len=strlen(d); if (len>4 && strcmp(d+len-4, ".chk")==0) {
                        char tag[256]; strncpy(tag,d,len-4); tag[len-4]='\0'; strcat(r.data,"--> "); strcat(r.data,tag); strcat(r.data,"\n"); }
                }
                closedir(dir);
            }
            send_message(client_socket,&r);
        } else if (msg.type == MSG_CP_REVERT) {
            char cpdir[800]; build_checkpoint_dir_for_file(msg.filename, cpdir, sizeof(cpdir));
            char cpsnap[900]; snprintf(cpsnap,sizeof(cpsnap),"%s/%s.chk", cpdir, msg.data);
            char* snap = read_file_content(cpsnap);
            if (!snap) { Message r={0}; r.type=MSG_ERROR; r.error_code=ERR_FILE_NOT_FOUND; strcpy(r.data,"ERROR: Checkpoint not found."); send_message(client_socket,&r); }
            else {
                // Write snapshot back
                char filepath[700];
                if (!resolve_existing_file_path(msg.filename, filepath, sizeof(filepath))) {
                    get_filepath(msg.filename, filepath);
                }
                atomic_write_file(filepath, snap);
                // Mirror revert to backup copy
                update_backup_for_file(msg.filename, snap);
                // Update metadata counts
                int word_count_total=0; int char_count_total=strlen(snap); char* temp=strdup(snap); char* t=strtok(temp," \t\n"); while(t){word_count_total++; t=strtok(NULL," \t\n");} free(temp);
                Message meta_msg={0}; meta_msg.type=MSG_UPDATE_METADATA; strcpy(meta_msg.filename,msg.filename); meta_msg.arg1=word_count_total; meta_msg.arg2=char_count_total; send_message(nm_socket_fd,&meta_msg); Message meta_ack; recv_message(nm_socket_fd,&meta_ack);
                free(snap);
                Message r={0}; r.type=MSG_ACK; strcpy(r.data,"Revert successful!"); send_message(client_socket,&r);
            }
        }
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
    // Optionally include current file list for seeding at NM
    // Build comma-separated list of regular files in this SS storage dir
    DIR* dir = opendir(storage_dir);
    if (dir) {
        struct dirent* entry;
        char listbuf[MAX_CONTENT_LEN] = "";
        bool first = true;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] == '.') continue; // skip . and .. and hidden
            // Only include regular files
#ifdef DT_REG
            if (entry->d_type != DT_REG) continue;
#endif
            if (!first) strncat(listbuf, ",", sizeof(listbuf) - strlen(listbuf) - 1);
            strncat(listbuf, entry->d_name, sizeof(listbuf) - strlen(listbuf) - 1);
            first = false;
        }
        closedir(dir);
        if (strlen(listbuf) > 0) {
            strncpy(reg_msg.data, listbuf, sizeof(reg_msg.data) - 1);
        }
    }
    
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
            case MSG_CREATE_FOLDER_SS: {
                // Create a physical folder under this SS's storage directory
                char path[600];
                snprintf(path, sizeof(path), "%s/%s", storage_dir, msg.data);
                mkdir(path, 0777);
                Message ack={0}; ack.type=MSG_ACK; strcpy(ack.data, "FOLDER_OK");
                send_message(nm_socket_fd, &ack);
                break;
            }
            case MSG_MOVE_FILE_SS: {
                // Move a file into the given folder: msg.filename, msg.data = folder
                char cur[700];
                if (!resolve_existing_file_path(msg.filename, cur, sizeof(cur))) {
                    // If not found, treat as error but ACK best-effort
                    Message ack={0}; ack.type=MSG_ERROR; strcpy(ack.data, "MOVE_NOT_FOUND"); send_message(nm_socket_fd,&ack);
                    break;
                }
                // Ensure target dir exists
                char dst_dir[600]; snprintf(dst_dir, sizeof(dst_dir), "%s/%s", storage_dir, msg.data);
                mkdir(dst_dir, 0777);
                char dst[800]; snprintf(dst, sizeof(dst), "%s/%s", dst_dir, msg.filename);
                // rename within same FS
                rename(cur, dst);
                Message ack={0}; ack.type=MSG_ACK; strcpy(ack.data, "MOVE_OK"); send_message(nm_socket_fd, &ack);
                break;
            }
            case MSG_WRITE_FILE_SS: {
                // Direct write content pushed by NM (for replication or recovery)
                char filepath[700];
                if (!resolve_existing_file_path(msg.filename, filepath, sizeof(filepath))) {
                    get_filepath(msg.filename, filepath);
                }
                atomic_write_file(filepath, msg.data);
                // Mirror replicated write to backup
                update_backup_for_file(msg.filename, msg.data);
                // Update metadata counts back to NM (best-effort)
                int word_count_total=0; int char_count_total=strlen(msg.data);
                char *tmp=strdup(msg.data); char *t=strtok(tmp, " \t\n"); while(t){word_count_total++; t=strtok(NULL," \t\n");} free(tmp);
                Message meta_msg={0}; meta_msg.type=MSG_UPDATE_METADATA; strcpy(meta_msg.filename,msg.filename); meta_msg.arg1=word_count_total; meta_msg.arg2=char_count_total; send_message(nm_socket_fd,&meta_msg); Message meta_ack; recv_message(nm_socket_fd,&meta_ack);
                Message ack={0}; ack.type=MSG_ACK; strcpy(ack.data, "WRITE_OK"); send_message(nm_socket_fd,&ack);
                break;
            }
            case MSG_SS_HEARTBEAT: {
                Message ack={0}; ack.type=MSG_ACK; strcpy(ack.data, "PONG"); send_message(nm_socket_fd,&ack);
                break;
            }
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
