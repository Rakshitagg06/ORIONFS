#include "../common/protocol.h"
#include "../common/utils.h"
#include "../common/network.h"
#include "nm_storage.h"
#include <pthread.h>
#include <signal.h>

#define NM_PORT 8080

// Global Trie root
TrieNode* file_trie = NULL;
pthread_mutex_t trie_mutex = PTHREAD_MUTEX_INITIALIZER;//initialised mutex 
static LRUCache* meta_cache = NULL; // LRU cache for FileMetadata

// Forward declarations
static char *fetch_from_ss_id(int ss_id, const char *file);
// For passing connection info to threads
typedef struct {
    int socket_fd;
    char ip[MAX_IP_LEN];
    int port;
} ConnectionArgs;

// Heartbeat thread for SS failure detection
static void* heartbeat_thread(void* arg) {
    (void)arg;
    for (;;) {
        for (int i=0; i<ss_count; ++i) {
            StorageServerInfo *ss = &ss_registry[i];
            if (!ss) continue;
            Message ping = {0}; ping.type = MSG_SS_HEARTBEAT;
            if (send_message(ss->socket_fd, &ping) < 0) { set_storage_server_status(ss->id, false); continue; }
            // short timeout
            set_socket_timeout(ss->socket_fd, 1);
            Message pong; int r = recv_message(ss->socket_fd, &pong);
            // restore default (blocking)
            set_socket_timeout(ss->socket_fd, 0);
            if (r > 0 && pong.type == MSG_ACK) set_storage_server_status(ss->id, true);
            else set_storage_server_status(ss->id, false);
        }
        sleep(5);
    }
    return NULL;
}

void handle_view_files(Message* msg, int client_socket) {
    ClientInfo* client = get_client_by_socket(client_socket);
    if (!client) return;
    
    char filenames[1000][MAX_FILENAME_LEN];
    int count = 0;
    
    pthread_mutex_lock(&trie_mutex);//lock the trie mutex
    
    if (msg->arg1) { // -a flag: all files
        count = trie_get_all_files(file_trie, filenames, 1000);
    } else {
        count = get_user_accessible_files(file_trie, client->username, filenames, 1000);
    }
    
    Message response = {0};
    response.type = MSG_FILE_LIST;
    
    if (msg->arg2) { // -l flag: with details
        char details[MAX_CONTENT_LEN] = "";
        strcat(details, "---------------------------------------------------------\n");
        strcat(details, "|  Filename  | Words | Chars | Last Access Time | Owner |\n");
        strcat(details, "|------------|-------|-------|------------------|-------|\n");
        
        for (int i = 0; i < count; i++) {
            FileMetadata* meta = trie_search(file_trie, filenames[i]);
            if (meta) {
                // Lazy-refresh metadata if counts are zero (keep VIEW -l informative)
                if ((meta->word_count == 0 || meta->char_count == 0)) {
                    // try to fetch content from SS and recompute counts
                    char *content = fetch_from_ss_id(meta->ss_id, meta->filename);
                    if (content) {
                        int wc = 0;
                        char* tmp = strdup(content);
                        char* tk = strtok(tmp, " \t\n");
                        while (tk) { wc++; tk = strtok(NULL, " \t\n"); }
                        free(tmp);
                        int cc = (int)strlen(content);
                        free(content);
                        meta->word_count = wc;
                        meta->char_count = cc;
                        meta->size_bytes = cc;
                        meta->last_modified = time(NULL);
                        trie_save_to_disk(file_trie, "metadata.dat");
                        cache_put(meta_cache, meta->filename, meta);
                    }
                }
                char line[256];
                char time_str[64];
                timestamp_to_string(meta->last_accessed, time_str, sizeof(time_str));
                snprintf(line, sizeof(line), "| %-10s | %5d | %5d | %16s | %-5s |\n",
                        meta->filename, meta->word_count, meta->char_count, time_str, meta->owner);
                strcat(details, line);
            }
        }
        strcat(details, "---------------------------------------------------------\n");
        strncpy(response.data, details, MAX_CONTENT_LEN - 1);
    } else {
        // Simple list
        for (int i = 0; i < count; i++) {
            strcat(response.data, "--> ");
            strcat(response.data, filenames[i]);
            strcat(response.data, "\n");
        }
    }
    
    pthread_mutex_unlock(&trie_mutex);
    
    send_message(client_socket, &response);
    log_message(LOG_INFO, "client", 0, client->username, "VIEW", "Success", "OK");
}

// Forward declare helpers
static void async_replicate_file(const char *filename, FileMetadata *metadata);

void handle_create_file(Message* msg, int client_socket) {
    ClientInfo* client = get_client_by_socket(client_socket);
    if (!client) return;
    
    Message response = {0};
    
    pthread_mutex_lock(&trie_mutex);
    
    // Check if file exists
    if (trie_search(file_trie, msg->filename) != NULL) {
        response.type = MSG_ERROR;
        response.error_code = ERR_FILE_EXISTS;
        strcpy(response.data, "ERROR: File already exists.");
        pthread_mutex_unlock(&trie_mutex);
        send_message(client_socket, &response);
        return;
    }
    
    // Select primary + replica storage servers
    StorageServerInfo* ss = select_storage_server_for_new_file();
    if (!ss) {
        response.type = MSG_ERROR;
        response.error_code = ERR_SS_UNAVAILABLE;
        strcpy(response.data, "No storage server available");
        pthread_mutex_unlock(&trie_mutex);
        send_message(client_socket, &response);
        return;
    }
    
    // Forward to primary SS
    Message ss_msg = {0};
    ss_msg.type = MSG_CREATE_FILE_SS;
    strcpy(ss_msg.filename, msg->filename);
    strcpy(ss_msg.sender, client->username);
    
    send_message(ss->socket_fd, &ss_msg);
    Message ss_response; recv_message(ss->socket_fd, &ss_response);
    
    if (ss_response.error_code == ERR_SUCCESS) {
        // Create metadata first
    FileMetadata* metadata = create_file_metadata(msg->filename, client->username,
                              ss->id, ss->ip, ss->client_port);
    trie_insert(file_trie, msg->filename, metadata);
    cache_put(meta_cache, msg->filename, metadata);
        
        // Save to disk
        trie_save_to_disk(file_trie, "metadata.dat");
        response.type = MSG_ACK;
        strcpy(response.data, "File Created Successfully!");
        log_message(LOG_INFO, "client", 0, client->username, "CREATE", msg->filename, "SUCCESS");
        // Send response to client immediately before attempting replica operations
        pthread_mutex_unlock(&trie_mutex);
        send_message(client_socket, &response);
        // Attempt replica creation asynchronously (best-effort, non-blocking for client)
        int replica_id = replicas_pick_alternate(ss->id);
        if (replica_id >= 0) {
            StorageServerInfo *rss = get_storage_server(replica_id);
            if (rss) {
                Message rmsg = ss_msg; send_message(rss->socket_fd, &rmsg);
                // Try to receive ACK with short timeout; restore after
                set_socket_timeout(rss->socket_fd, 1);
                Message rack; recv_message(rss->socket_fd, &rack);
                set_socket_timeout(rss->socket_fd, 0);
                if (rack.type == MSG_ACK) {
                    int rids[1] = { replica_id }; replicas_set(msg->filename, ss->id, rids, 1);
                } else {
                    log_message(LOG_WARNING, rss->ip, rss->client_port, client->username, "REPLICA_CREATE", msg->filename, "TIMEOUT_OR_FAIL");
                }
            }
        }
        return; // already responded
    } else {
        response.type = MSG_ERROR;
        response.error_code = ss_response.error_code;
        strcpy(response.data, "ERROR: Failed to create file.");
    }
    pthread_mutex_unlock(&trie_mutex);
    send_message(client_socket, &response);
}

void handle_read_file(Message* msg, int client_socket) {
    ClientInfo* client = get_client_by_socket(client_socket);
    if (!client) return;
    
    Message response = {0};
    
    pthread_mutex_lock(&trie_mutex);
    // Try cache first
    FileMetadata* metadata = cache_get(meta_cache, msg->filename);
    if (!metadata) {
        metadata = trie_search(file_trie, msg->filename);
        if (metadata) cache_put(meta_cache, msg->filename, metadata);
    }
    
    if (!metadata) {
        response.type = MSG_ERROR;
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "ERROR: File not found.");
        pthread_mutex_unlock(&trie_mutex);
        send_message(client_socket, &response);
        return;
    }
    
    if (check_file_access(metadata, client->username) == ACCESS_NONE) {
        response.type = MSG_ERROR;
        response.error_code = ERR_ACCESS_DENIED;
        strcpy(response.data, "ERROR: Access denied.");
        pthread_mutex_unlock(&trie_mutex);
        send_message(client_socket, &response);
        return;
    }
    
    // Return SS info (fallback to replica if primary is down)
    int ssid = metadata->ss_id;
    int pick = replicas_pick_alive_for_access(metadata->filename, ssid);
    StorageServerInfo *target = (pick>=0) ? get_storage_server(pick) : get_storage_server(ssid);
    response.type = MSG_SS_INFO;
    if (target) {
        strcpy(response.data, target->ip);
        response.arg1 = target->client_port;
    } else {
        response.type = MSG_ERROR; response.error_code=ERR_SS_UNAVAILABLE; strcpy(response.data,"ERROR: Storage server unavailable.");
        pthread_mutex_unlock(&trie_mutex);
        send_message(client_socket, &response);
        return;
    }
    
    metadata->last_accessed = time(NULL);
    strcpy(metadata->last_accessed_by, client->username);
    
    pthread_mutex_unlock(&trie_mutex);
    send_message(client_socket, &response);
    log_message(LOG_INFO, "client", 0, client->username, "READ", msg->filename, "SS_INFO_SENT");
}

// Same as handle_read_file, but logs operation as STREAM so it shows up in NM logs
void handle_stream_lookup(Message* msg, int client_socket) {
    ClientInfo* client = get_client_by_socket(client_socket);
    if (!client) return;

    Message response = {0};

    pthread_mutex_lock(&trie_mutex);
    // Try cache first
    FileMetadata* metadata = cache_get(meta_cache, msg->filename);
    if (!metadata) {
        metadata = trie_search(file_trie, msg->filename);
        if (metadata) cache_put(meta_cache, msg->filename, metadata);
    }

    if (!metadata) {
        response.type = MSG_ERROR;
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "ERROR: File not found.");
        pthread_mutex_unlock(&trie_mutex);
        send_message(client_socket, &response);
        return;
    }

    if (check_file_access(metadata, client->username) == ACCESS_NONE) {
        response.type = MSG_ERROR;
        response.error_code = ERR_ACCESS_DENIED;
        strcpy(response.data, "ERROR: Access denied.");
        pthread_mutex_unlock(&trie_mutex);
        send_message(client_socket, &response);
        return;
    }

    // Return SS info (fallback to replica if primary is down)
    int ssid = metadata->ss_id;
    int pick = replicas_pick_alive_for_access(metadata->filename, ssid);
    StorageServerInfo *target = (pick>=0) ? get_storage_server(pick) : get_storage_server(ssid);
    response.type = MSG_SS_INFO;
    if (target) {
        strcpy(response.data, target->ip);
        response.arg1 = target->client_port;
    } else {
        response.type = MSG_ERROR; response.error_code=ERR_SS_UNAVAILABLE; strcpy(response.data,"ERROR: Storage server unavailable.");
        pthread_mutex_unlock(&trie_mutex);
        send_message(client_socket, &response);
        return;
    }

    metadata->last_accessed = time(NULL);
    strcpy(metadata->last_accessed_by, client->username);

    pthread_mutex_unlock(&trie_mutex);
    send_message(client_socket, &response);
    log_message(LOG_INFO, "client", 0, client->username, "STREAM", msg->filename, "SS_INFO_SENT");
}

// UNDO lookup: identical to READ lookup but logs action as UNDO for clarity in NM logs
void handle_undo_lookup(Message* msg, int client_socket) {
    ClientInfo* client = get_client_by_socket(client_socket);
    if (!client) return;

    Message response = {0};

    pthread_mutex_lock(&trie_mutex);
    FileMetadata* metadata = cache_get(meta_cache, msg->filename);
    if (!metadata) {
        metadata = trie_search(file_trie, msg->filename);
        if (metadata) cache_put(meta_cache, msg->filename, metadata);
    }

    if (!metadata) {
        response.type = MSG_ERROR;
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "ERROR: File not found.");
        pthread_mutex_unlock(&trie_mutex);
        send_message(client_socket, &response);
        return;
    }

    if (check_file_access(metadata, client->username) == ACCESS_NONE) {
        response.type = MSG_ERROR;
        response.error_code = ERR_ACCESS_DENIED;
        strcpy(response.data, "ERROR: Access denied.");
        pthread_mutex_unlock(&trie_mutex);
        send_message(client_socket, &response);
        return;
    }

    int ssid = metadata->ss_id;
    int pick = replicas_pick_alive_for_access(metadata->filename, ssid);
    StorageServerInfo *target = (pick>=0) ? get_storage_server(pick) : get_storage_server(ssid);
    response.type = MSG_SS_INFO;
    if (target) {
        strcpy(response.data, target->ip);
        response.arg1 = target->client_port;
    } else {
        response.type = MSG_ERROR; response.error_code=ERR_SS_UNAVAILABLE; strcpy(response.data,"ERROR: Storage server unavailable.");
        pthread_mutex_unlock(&trie_mutex);
        send_message(client_socket, &response);
        return;
    }

    metadata->last_accessed = time(NULL);
    strcpy(metadata->last_accessed_by, client->username);
    pthread_mutex_unlock(&trie_mutex);
    send_message(client_socket, &response);
    log_message(LOG_INFO, "client", 0, client->username, "UNDO", msg->filename, "SS_INFO_SENT");
}

void handle_write_file(Message* msg, int client_socket) {
    ClientInfo* client = get_client_by_socket(client_socket);
    if (!client) return;
    
    Message response = {0};
    
    pthread_mutex_lock(&trie_mutex);
    FileMetadata* metadata = cache_get(meta_cache, msg->filename);
    if (!metadata) {
        metadata = trie_search(file_trie, msg->filename);
        if (metadata) cache_put(meta_cache, msg->filename, metadata);
    }
    
    if (!metadata) {
        response.type = MSG_ERROR;
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "ERROR: File not found.");
        pthread_mutex_unlock(&trie_mutex);
        send_message(client_socket, &response);
        return;
    }
    
    AccessType access = check_file_access(metadata, client->username);
    if (access != ACCESS_WRITE) {
        response.type = MSG_ERROR;
        response.error_code = ERR_ACCESS_DENIED;
        strcpy(response.data, "ERROR: Access denied.");
        pthread_mutex_unlock(&trie_mutex);
        send_message(client_socket, &response);
        return;
    }
    
    // Return SS info (fallback to replica if primary is down)
    int ssid = metadata->ss_id;
    int pick = replicas_pick_alive_for_access(metadata->filename, ssid);
    StorageServerInfo *target = (pick>=0) ? get_storage_server(pick) : get_storage_server(ssid);
    response.type = MSG_SS_INFO;
    if (target) {
        strcpy(response.data, target->ip);
        response.arg1 = target->client_port;
    } else {
        response.type = MSG_ERROR; response.error_code=ERR_SS_UNAVAILABLE; strcpy(response.data,"ERROR: Storage server unavailable.");
        pthread_mutex_unlock(&trie_mutex);
        send_message(client_socket, &response);
        return;
    }
    
    pthread_mutex_unlock(&trie_mutex);
    send_message(client_socket, &response);
    log_message(LOG_INFO, "client", 0, client->username, "WRITE", msg->filename, "SS_INFO_SENT");
}

void handle_delete_file(Message* msg, int client_socket) {
    ClientInfo* client = get_client_by_socket(client_socket);
    if (!client) return;
    
    Message response = {0};
    
    pthread_mutex_lock(&trie_mutex);
    FileMetadata* metadata = cache_get(meta_cache, msg->filename);
    if (!metadata) {
        metadata = trie_search(file_trie, msg->filename);
        if (metadata) cache_put(meta_cache, msg->filename, metadata);
    }
    
    if (!metadata) {
        response.type = MSG_ERROR;
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "ERROR: File not found.");
        pthread_mutex_unlock(&trie_mutex);
        send_message(client_socket, &response);
        return;
    }
    
    if (strcmp(metadata->owner, client->username) != 0) {
        response.type = MSG_ERROR;
        response.error_code = ERR_NOT_OWNER;
        strcpy(response.data, "ERROR: Only owner can delete file.");
        pthread_mutex_unlock(&trie_mutex);
        send_message(client_socket, &response);
        return;
    }
    
    // Forward to SS
    StorageServerInfo* ss = get_storage_server(metadata->ss_id);
    Message ss_msg = {0};
    ss_msg.type = MSG_DELETE_FILE_SS;
    strcpy(ss_msg.filename, msg->filename);
    
    send_message(ss->socket_fd, &ss_msg);
    
    Message ss_response;
    recv_message(ss->socket_fd, &ss_response);
    
    if (ss_response.error_code == ERR_SUCCESS) {
    trie_delete(file_trie, msg->filename);
    cache_remove(meta_cache, msg->filename);
        
        // Save to disk
        trie_save_to_disk(file_trie, "metadata.dat");
        
    response.type = MSG_ACK;
    snprintf(response.data, sizeof(response.data), "File '%s' deleted successfully!", msg->filename);
        log_message(LOG_INFO, "client", 0, client->username, "DELETE", msg->filename, "SUCCESS");
    } else {
        response.type = MSG_ERROR;
        strcpy(response.data, "ERROR: Failed to delete file.");
    }
    
    pthread_mutex_unlock(&trie_mutex);
    send_message(client_socket, &response);
}

void handle_info_file(Message* msg, int client_socket) {
    ClientInfo* client = get_client_by_socket(client_socket);
    if (!client) return;
    
    Message response = {0};
    
    pthread_mutex_lock(&trie_mutex);
    FileMetadata* metadata = cache_get(meta_cache, msg->filename);
    if (!metadata) {
        metadata = trie_search(file_trie, msg->filename);
        if (metadata) cache_put(meta_cache, msg->filename, metadata);
    }
    
    if (!metadata) {
        response.type = MSG_ERROR;
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "ERROR: File not found.");
        pthread_mutex_unlock(&trie_mutex);
        send_message(client_socket, &response);
        return;
    }
    
    char info[MAX_CONTENT_LEN];
    char time_str[64];
    
    sprintf(info, "--> File: %s\n", metadata->filename);
    sprintf(info + strlen(info), "--> Owner: %s\n", metadata->owner);
    
    timestamp_to_string(metadata->created_at, time_str, sizeof(time_str));
    sprintf(info + strlen(info), "--> Created: %s\n", time_str);
    
    timestamp_to_string(metadata->last_modified, time_str, sizeof(time_str));
    sprintf(info + strlen(info), "--> Last Modified: %s\n", time_str);
    
    sprintf(info + strlen(info), "--> Size: %zu bytes\n", metadata->size_bytes);
    sprintf(info + strlen(info), "--> Access: ");
    
    for (int i = 0; i < metadata->access_count; i++) {
        sprintf(info + strlen(info), "%s (%s)", 
                metadata->access_list[i].username,
                metadata->access_list[i].access == ACCESS_WRITE ? "RW" : "R");
        if (i < metadata->access_count - 1) strcat(info, ", ");
    }
    strcat(info, "\n");
    
    timestamp_to_string(metadata->last_accessed, time_str, sizeof(time_str));
    sprintf(info + strlen(info), "--> Last Accessed: %s by %s\n", 
            time_str, metadata->last_accessed_by);
    
    response.type = MSG_FILE_INFO;
    strcpy(response.data, info);
    
    pthread_mutex_unlock(&trie_mutex);
    send_message(client_socket, &response);
}

void handle_list_users(Message* msg, int client_socket) {
    Message response = {0};
    response.type = MSG_USER_LIST;
    
    char usernames[MAX_CLIENTS][MAX_USERNAME_LEN];
    int count = get_all_usernames(usernames, MAX_CLIENTS);
    
    for (int i = 0; i < count; i++) {
        strcat(response.data, "--> ");
        strcat(response.data, usernames[i]);
        strcat(response.data, "\n");
    }
    
    send_message(client_socket, &response);
}

static int fetch_file_content_from_ss(FileMetadata* metadata, char* out, size_t out_sz) {
    if (!metadata) return -1;
    int ss_socket = connect_to_server(metadata->ss_ip, metadata->ss_client_port);
    if (ss_socket < 0) return -1;
    Message req = (Message){0};
    req.type = MSG_READ_REQUEST;
    strcpy(req.sender, "nm");
    strcpy(req.filename, metadata->filename);
    send_message(ss_socket, &req);
    Message resp; int ok = -1;
    if (recv_message(ss_socket, &resp) > 0 && resp.type == MSG_ACK) {
        strncpy(out, resp.data, out_sz - 1);
        out[out_sz - 1] = '\0';
        ok = 0;
    }
    close_socket(ss_socket);
    return ok;
}

// Specialized fetch for EXEC so storage server can log EXEC instead of READ
static int fetch_file_content_for_exec(FileMetadata* metadata, char* out, size_t out_sz) {
    if (!metadata) return -1;
    int ss_socket = connect_to_server(metadata->ss_ip, metadata->ss_client_port);
    if (ss_socket < 0) return -1;
    Message req = (Message){0};
    req.type = MSG_EXEC_READ_REQUEST;
    strcpy(req.sender, "nm");
    strcpy(req.filename, metadata->filename);
    send_message(ss_socket, &req);
    Message resp; int ok = -1;
    if (recv_message(ss_socket, &resp) > 0 && resp.type == MSG_ACK) {
        strncpy(out, resp.data, out_sz - 1);
        out[out_sz - 1] = '\0';
        ok = 0;
    }
    close_socket(ss_socket);
    return ok;
}

void handle_exec_file(Message* msg, int client_socket) {
    ClientInfo* client = get_client_by_socket(client_socket);
    if (!client) return;
    Message response = {0};

    pthread_mutex_lock(&trie_mutex);
    FileMetadata* metadata = cache_get(meta_cache, msg->filename);
    if (!metadata) {
        metadata = trie_search(file_trie, msg->filename);
        if (metadata) cache_put(meta_cache, msg->filename, metadata);
    }
    if (!metadata) {
        response.type = MSG_ERROR;
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "ERROR: File not found.");
        pthread_mutex_unlock(&trie_mutex);
        send_message(client_socket, &response);
        return;
    }
    // Require READ access to execute
    if (check_file_access(metadata, client->username) == ACCESS_NONE) {
        response.type = MSG_ERROR;
        response.error_code = ERR_ACCESS_DENIED;
        strcpy(response.data, "ERROR: Access denied.");
        pthread_mutex_unlock(&trie_mutex);
        send_message(client_socket, &response);
        return;
    }
    pthread_mutex_unlock(&trie_mutex);

    // Fetch content via EXEC-specific request so SS logs EXEC
    char filebuf[MAX_CONTENT_LEN];
    if (fetch_file_content_for_exec(metadata, filebuf, sizeof(filebuf)) != 0) {
        response.type = MSG_ERROR;
        response.error_code = ERR_UNKNOWN;
        strcpy(response.data, "ERROR: Failed to fetch file for execution.");
        send_message(client_socket, &response);
        log_message(LOG_ERROR, "client", 0, client->username, "EXEC", msg->filename, "FETCH_FAILED");
        return;
    }

    // Log execution start
    log_message(LOG_INFO, "client", 0, client->username, "EXEC", msg->filename, "START");

    // Execute content as a shell command using popen
    FILE* pipe = popen(filebuf, "r");
    if (!pipe) {
        response.type = MSG_ERROR;
        response.error_code = ERR_UNKNOWN;
        strcpy(response.data, "ERROR: Failed to execute file.");
        send_message(client_socket, &response);
        return;
    }

    char outbuf[512];
    while (fgets(outbuf, sizeof(outbuf), pipe)) {
        Message out = {0};
        out.type = MSG_EXEC_OUTPUT;
        strncpy(out.data, outbuf, sizeof(out.data) - 1);
        send_message(client_socket, &out);
    }
    pclose(pipe);

    Message done = {0};
    done.type = MSG_EXEC_DONE;
    strcpy(done.data, "END_OF_EXEC");
    send_message(client_socket, &done);
    // Log success
    log_message(LOG_INFO, "client", 0, client->username, "EXEC", msg->filename, "SUCCESS");
}

void handle_add_access(Message* msg, int client_socket) {
    ClientInfo* client = get_client_by_socket(client_socket);
    if (!client) return;
    
    Message response = {0};
    
    pthread_mutex_lock(&trie_mutex);
    FileMetadata* metadata = cache_get(meta_cache, msg->filename);
    if (!metadata) {
        metadata = trie_search(file_trie, msg->filename);
        if (metadata) cache_put(meta_cache, msg->filename, metadata);
    }
    
    if (!metadata) {
        response.type = MSG_ERROR;
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "ERROR: File not found.");
        pthread_mutex_unlock(&trie_mutex);
        send_message(client_socket, &response);
        return;
    }
    
    if (strcmp(metadata->owner, client->username) != 0) {
        response.type = MSG_ERROR;
        response.error_code = ERR_NOT_OWNER;
        strcpy(response.data, "ERROR: Access denied.");
        pthread_mutex_unlock(&trie_mutex);
        send_message(client_socket, &response);
        return;
    }
    
    AccessType access = (msg->arg1 == 1) ? ACCESS_READ : ACCESS_WRITE;
    add_file_access(metadata, msg->data, access);
    
    response.type = MSG_ACK;
    strcpy(response.data, "Access granted successfully!");
    // persist metadata after ACL change
    trie_save_to_disk(file_trie, "metadata.dat");
    
    pthread_mutex_unlock(&trie_mutex);
    send_message(client_socket, &response);
    log_message(LOG_INFO, "client", 0, client->username, "ADDACCESS", msg->filename, "SUCCESS");
}

void handle_create_folder(Message* msg, int client_socket) {
    ClientInfo* client = get_client_by_socket(client_socket);
    if (!client) return;
    Message response = {0};
    const char* folder = msg->data;
    if (!folder || !*folder) {
        response.type = MSG_ERROR;
        strcpy(response.data, "ERROR: Invalid folder name.");
        send_message(client_socket, &response);
        return;
    }
    if (create_folder(folder) == 0) {
        folders_save_to_disk("folders.dat");
        // Broadcast physical folder creation to all storage servers (best-effort)
        for (int i = 0; i < ss_count; ++i) {
            StorageServerInfo *ss = &ss_registry[i];
            if (!ss) continue;
            Message fmsg = {0};
            fmsg.type = MSG_CREATE_FOLDER_SS;
            strncpy(fmsg.data, folder, sizeof(fmsg.data)-1);
            // fire-and-forget; ignore errors
            if (ss->socket_fd > 0) {
                send_message(ss->socket_fd, &fmsg);
                // Optionally read ACK but don't block overall flow
                // set short timeout to avoid stalling
                set_socket_timeout(ss->socket_fd, 1);
                Message ack; recv_message(ss->socket_fd, &ack);
                set_socket_timeout(ss->socket_fd, 0);
            }
        }
        response.type = MSG_ACK;
        strcpy(response.data, "Folder created successfully!");
    } else {
        response.type = MSG_ERROR;
        strcpy(response.data, "ERROR: Failed to create folder.");
    }
    send_message(client_socket, &response);
    log_message(LOG_INFO, client->ip, client->conn_port, client->username, "CREATEFOLDER", folder, (response.type==MSG_ACK)?"SUCCESS":"FAIL");
}

void handle_move_file(Message* msg, int client_socket) {
    ClientInfo* client = get_client_by_socket(client_socket);
    if (!client) return;
    Message response = {0};
    const char* filename = msg->filename;
    const char* folder = msg->data;

    pthread_mutex_lock(&trie_mutex);
    FileMetadata* metadata = cache_get(meta_cache, filename);
    if (!metadata) {
        metadata = trie_search(file_trie, filename);
        if (metadata) cache_put(meta_cache, filename, metadata);
    }
    if (!metadata) {
        pthread_mutex_unlock(&trie_mutex);
        response.type = MSG_ERROR;
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "ERROR: File not found.");
        send_message(client_socket, &response);
        return;
    }
    pthread_mutex_unlock(&trie_mutex);

    if (!folder || !*folder) {
        response.type = MSG_ERROR;
        strcpy(response.data, "ERROR: Invalid folder name.");
        send_message(client_socket, &response);
        return;
    }
    if (move_file_to_folder(filename, folder) == 0) {
        folders_save_to_disk("folders.dat");
        // Broadcast physical move to primary and replicas
        // Need metadata to know primary
        pthread_mutex_lock(&trie_mutex);
        FileMetadata* meta2 = trie_search(file_trie, filename);
        pthread_mutex_unlock(&trie_mutex);
        if (meta2) {
            Message m = {0};
            m.type = MSG_MOVE_FILE_SS;
            strncpy(m.filename, filename, sizeof(m.filename)-1);
            strncpy(m.data, folder, sizeof(m.data)-1);
            // primary
            StorageServerInfo *primary = get_storage_server(meta2->ss_id);
            if (primary && primary->socket_fd > 0) {
                send_message(primary->socket_fd, &m);
                set_socket_timeout(primary->socket_fd, 1); Message ack; recv_message(primary->socket_fd, &ack); set_socket_timeout(primary->socket_fd, 0);
            }
            // replicas
            int rids[MAX_STORAGE_SERVERS]; int n = replicas_get(filename, rids, MAX_STORAGE_SERVERS);
            for (int i=0;i<n;i++) {
                StorageServerInfo *rss = get_storage_server(rids[i]);
                if (!rss || rss->socket_fd <= 0) continue;
                send_message(rss->socket_fd, &m);
                set_socket_timeout(rss->socket_fd, 1); Message ack; recv_message(rss->socket_fd, &ack); set_socket_timeout(rss->socket_fd, 0);
            }
        }
        response.type = MSG_ACK;
        strcpy(response.data, "File moved successfully!");
    } else {
        response.type = MSG_ERROR;
        strcpy(response.data, "ERROR: Failed to move file.");
    }
    send_message(client_socket, &response);
    log_message(LOG_INFO, client->ip, client->conn_port, client->username, "MOVE", filename, (response.type==MSG_ACK)?"SUCCESS":"FAIL");
}

void handle_view_folder(Message* msg, int client_socket) {
    ClientInfo* client = get_client_by_socket(client_socket);
    if (!client) return;
    Message response = {0};
    const char* folder = msg->data;
    if (!folder || !*folder) {
        response.type = MSG_ERROR;
        strcpy(response.data, "ERROR: Invalid folder name.");
        send_message(client_socket, &response);
        return;
    }
    char files[1000][MAX_FILENAME_LEN];
    int count = list_folder_files(folder, files, 1000);
    response.type = MSG_FILE_LIST;
    if (count == 0) {
        strcpy(response.data, "");
    } else {
        for (int i = 0; i < count; ++i) {
            strcat(response.data, "--> ");
            strcat(response.data, files[i]);
            strcat(response.data, "\n");
        }
    }
    send_message(client_socket, &response);
    log_message(LOG_INFO, client->ip, client->conn_port, client->username, "VIEWFOLDER", folder, "OK");
}

void handle_remove_access(Message* msg, int client_socket) {
    ClientInfo* client = get_client_by_socket(client_socket);
    if (!client) return;
    Message response = {0};
    pthread_mutex_lock(&trie_mutex);
    FileMetadata* metadata = cache_get(meta_cache, msg->filename);
    if (!metadata) {
        metadata = trie_search(file_trie, msg->filename);
        if (metadata) cache_put(meta_cache, msg->filename, metadata);
    }
    if (!metadata) {
        response.type = MSG_ERROR;
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "ERROR: File not found.");
        pthread_mutex_unlock(&trie_mutex);
        send_message(client_socket, &response);
        return;
    }
    if (strcmp(metadata->owner, client->username) != 0) {
        response.type = MSG_ERROR;
        response.error_code = ERR_NOT_OWNER;
        strcpy(response.data, "ERROR: Access denied.");
        pthread_mutex_unlock(&trie_mutex);
        send_message(client_socket, &response);
        return;
    }
    remove_file_access(metadata, msg->data);
    response.type = MSG_ACK;
    strcpy(response.data, "Access removed successfully!");
    trie_save_to_disk(file_trie, "metadata.dat");
    pthread_mutex_unlock(&trie_mutex);
    send_message(client_socket, &response);
    log_message(LOG_INFO, client->ip, client->conn_port, client->username, "REMACCESS", msg->filename, "SUCCESS");
}

// Generic checkpoint access routing
static void handle_checkpoint_request(Message* msg, int client_socket) {
    ClientInfo* client = get_client_by_socket(client_socket);
    if (!client) return;
    Message response = {0};
    pthread_mutex_lock(&trie_mutex);
    FileMetadata* metadata = cache_get(meta_cache, msg->filename);
    if (!metadata) {
        metadata = trie_search(file_trie, msg->filename);
        if (metadata) cache_put(meta_cache, msg->filename, metadata);
    }
    if (!metadata) {
        response.type = MSG_ERROR;
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "ERROR: File not found.");
        pthread_mutex_unlock(&trie_mutex);
        send_message(client_socket, &response);
        return;
    }
    AccessType access = check_file_access(metadata, client->username);
    bool need_write = (msg->type == MSG_CP_REVERT);
    if ((need_write && access != ACCESS_WRITE) || (!need_write && access == ACCESS_NONE)) {
        response.type = MSG_ERROR;
        response.error_code = ERR_ACCESS_DENIED;
        strcpy(response.data, "ERROR: Access denied.");
        pthread_mutex_unlock(&trie_mutex);
        send_message(client_socket, &response);
        return;
    }
    response.type = MSG_SS_INFO;
    strcpy(response.data, metadata->ss_ip);
    response.arg1 = metadata->ss_client_port;
    pthread_mutex_unlock(&trie_mutex);
    send_message(client_socket, &response);
}

void* handle_client_thread(void* arg) {
    ConnectionArgs* cargs = (ConnectionArgs*)arg;
    int client_socket = cargs->socket_fd;
    char client_ip[MAX_IP_LEN];
    strncpy(client_ip, cargs->ip, sizeof(client_ip)-1);
    int client_port = cargs->port;
    free(cargs);
    
    while (1) {
        Message msg;
        int received = recv_message(client_socket, &msg);
        
        if (received <= 0) {
            remove_client(client_socket);
            close_socket(client_socket);
            break;
        }
        
        switch (msg.type) {
            case MSG_CP_CREATE:
            case MSG_CP_VIEW:
            case MSG_CP_REVERT:
            case MSG_CP_LIST:
                handle_checkpoint_request(&msg, client_socket);
                break;
            case MSG_CLIENT_REGISTER: {
                ClientInfo client_info = (ClientInfo){0};
                client_info.socket_fd = client_socket;
                strcpy(client_info.username, msg.sender);
                // Prefer actual network IP if available
                if (strlen(msg.data)) {
                    strncpy(client_info.ip, msg.data, sizeof(client_info.ip) - 1);
                } else {
                    strncpy(client_info.ip, client_ip, sizeof(client_info.ip) - 1);
                }
                client_info.nm_port = msg.arg1; // Provided NM port
                client_info.ss_port = msg.arg2; // Provided SS port (may be 0)
                client_info.conn_port = client_port; // Actual TCP source port
                client_info.is_connected = true;
                client_info.connected_at = time(NULL);
                register_client(&client_info);
                
                Message response = {0};
                response.type = MSG_ACK;
                strcpy(response.data, "Registration successful");
                send_message(client_socket, &response);
                log_message(LOG_INFO, client_info.ip, client_info.conn_port, msg.sender, "REGISTER", "Client registered", "SUCCESS");
                break;
            }
            case MSG_VIEW_FILES:
                handle_view_files(&msg, client_socket);
                break;
            case MSG_CREATE_FILE:
                handle_create_file(&msg, client_socket);
                break;
            case MSG_READ_FILE:
                handle_read_file(&msg, client_socket);
                break;
            case MSG_WRITE_FILE:
                handle_write_file(&msg, client_socket);
                break;
            case MSG_DELETE_FILE:
                handle_delete_file(&msg, client_socket);
                break;
            case MSG_INFO_FILE:
                handle_info_file(&msg, client_socket);
                break;
            case MSG_LIST_USERS:
                handle_list_users(&msg, client_socket);
                break;
            case MSG_ADDACCESS:
                handle_add_access(&msg, client_socket);
                break;
            case MSG_REMACCESS:
                handle_remove_access(&msg, client_socket);
                break;
            case MSG_EXEC_FILE:
                handle_exec_file(&msg, client_socket);
                break;
            case MSG_CREATE_FOLDER:
                handle_create_folder(&msg, client_socket);
                break;
            case MSG_MOVE_FILE:
                handle_move_file(&msg, client_socket);
                break;
            case MSG_VIEW_FOLDER:
                handle_view_folder(&msg, client_socket);
                break;
            case MSG_REQUEST_ACCESS: {
                Message response={0};
                pthread_mutex_lock(&trie_mutex);
                FileMetadata* m = cache_get(meta_cache, msg.filename);
                if (!m) { m = trie_search(file_trie, msg.filename); if (m) cache_put(meta_cache, msg.filename, m); }
                if (!m) {
                    response.type=MSG_ERROR; response.error_code=ERR_FILE_NOT_FOUND; strcpy(response.data, "ERROR: File not found.");
                } else if (strcmp(m->owner, msg.sender)==0) {
                    response.type=MSG_ERROR; response.error_code=ERR_INVALID_COMMAND; strcpy(response.data, "ERROR: Owner already has access.");
                } else if (check_file_access(m, msg.sender) >= (msg.arg1==2?ACCESS_WRITE:ACCESS_READ)) {
                    response.type=MSG_ERROR; response.error_code=ERR_INVALID_COMMAND; strcpy(response.data, "ERROR: Already has access.");
                } else {
                    pthread_mutex_unlock(&trie_mutex);
                    int rc = access_requests_append(msg.filename, msg.sender, (msg.arg1==2?ACCESS_WRITE:ACCESS_READ));
                    pthread_mutex_lock(&trie_mutex);
                    if (rc == -2) { response.type=MSG_ERROR; response.error_code=ERR_REQUEST_ALREADY_EXISTS; strcpy(response.data, "ERROR: Request already pending."); }
                    else if (rc < 0) { response.type=MSG_ERROR; response.error_code=ERR_UNKNOWN; strcpy(response.data, "ERROR: Failed to queue request."); }
                    else { response.type=MSG_ACK; strcpy(response.data, "Access request queued."); }
                }
                pthread_mutex_unlock(&trie_mutex);
                send_message(client_socket, &response);
                break;
            }
            case MSG_VIEW_REQUESTS: {
                Message response={0}; char buf[MAX_CONTENT_LEN];
                int n = access_requests_list_for_owner(msg.sender, buf, sizeof(buf));
                response.type = MSG_ACK; if (n==0) strcpy(buf, "No pending requests.\n");
                strncpy(response.data, buf, sizeof(response.data)-1);
                send_message(client_socket, &response);
                break;
            }
            case MSG_APPROVE_REQUEST: {
                Message response={0};
                pthread_mutex_lock(&trie_mutex);
                FileMetadata* m = cache_get(meta_cache, msg.filename);
                if (!m) { m = trie_search(file_trie, msg.filename); if (m) cache_put(meta_cache, msg.filename, m); }
                if (!m) { response.type=MSG_ERROR; response.error_code=ERR_FILE_NOT_FOUND; strcpy(response.data, "ERROR: File not found."); }
                else if (strcmp(m->owner, msg.sender)!=0) { response.type=MSG_ERROR; response.error_code=ERR_NOT_OWNER; strcpy(response.data, "ERROR: Not owner."); }
                pthread_mutex_unlock(&trie_mutex);
                if (response.type==0) {
                    int rc = access_requests_resolve(msg.filename, msg.data, true, (msg.arg1==2?ACCESS_WRITE:ACCESS_READ));
                    if (rc==0) { response.type=MSG_ACK; strcpy(response.data, "Request approved."); }
                    else { response.type=MSG_ERROR; response.error_code=ERR_REQUEST_NOT_FOUND; strcpy(response.data, "ERROR: Request not found."); }
                }
                send_message(client_socket, &response);
                break;
            }
            case MSG_DENY_REQUEST: {
                Message response={0};
                pthread_mutex_lock(&trie_mutex);
                FileMetadata* m = cache_get(meta_cache, msg.filename);
                if (!m) { m = trie_search(file_trie, msg.filename); if (m) cache_put(meta_cache, msg.filename, m); }
                if (!m) { response.type=MSG_ERROR; response.error_code=ERR_FILE_NOT_FOUND; strcpy(response.data, "ERROR: File not found."); }
                else if (strcmp(m->owner, msg.sender)!=0) { response.type=MSG_ERROR; response.error_code=ERR_NOT_OWNER; strcpy(response.data, "ERROR: Not owner."); }
                pthread_mutex_unlock(&trie_mutex);
                if (response.type==0) {
                    int rc = access_requests_resolve(msg.filename, msg.data, false, ACCESS_READ);
                    if (rc==0) { response.type=MSG_ACK; strcpy(response.data, "Request denied."); }
                    else { response.type=MSG_ERROR; response.error_code=ERR_REQUEST_NOT_FOUND; strcpy(response.data, "ERROR: Request not found."); }
                }
                send_message(client_socket, &response);
                break;
            }
            case MSG_STREAM_FILE:
                handle_stream_lookup(&msg, client_socket); // Log as STREAM and return SS info
                break;
            case MSG_UNDO_FILE:
                handle_undo_lookup(&msg, client_socket);
                break;
            default:
                break;
        }
    }
    
    return NULL;
}

void* handle_ss_thread(void* arg) {
    ConnectionArgs* cargs = (ConnectionArgs*)arg;
    int ss_socket = cargs->socket_fd;
    char ss_ip[MAX_IP_LEN];
    strncpy(ss_ip, cargs->ip, sizeof(ss_ip)-1);
    int ss_conn_port = cargs->port;
    free(cargs);
    
    // Receive registration
    Message reg_msg;
    recv_message(ss_socket, &reg_msg);
    
    if (reg_msg.type == MSG_SS_REGISTER) {
    StorageServerInfo ss_info = {0};
        // Use actual peer IP from accept
        strncpy(ss_info.ip, ss_ip, sizeof(ss_info.ip)-1);
        ss_info.nm_port = reg_msg.arg1;
        ss_info.client_port = reg_msg.arg2;
    ss_info.is_alive = true;
        ss_info.socket_fd = ss_socket;
        
    int ss_id = register_storage_server(&ss_info);
        
        Message response = {0};
        response.type = MSG_ACK;
        sprintf(response.data, "Storage Server %d registered", ss_id);
        send_message(ss_socket, &response);
        
    log_message(LOG_INFO, ss_info.ip, ss_conn_port, reg_msg.sender, "REGISTER", "SS registered", "SUCCESS");
        printf("✅ Storage Server %d registered\n", ss_id);

    // Optional seeding: SS can send a comma-separated file list in reg_msg.data
        if (strlen(reg_msg.data) > 0) {
            char listbuf[MAX_CONTENT_LEN];
            strncpy(listbuf, reg_msg.data, sizeof(listbuf) - 1);
            listbuf[sizeof(listbuf) - 1] = '\0';
            char* saveptr = NULL;
            char* tok = strtok_r(listbuf, ",", &saveptr);
            int seeded = 0;
            pthread_mutex_lock(&trie_mutex);
            while (tok) {
                // trim leading/trailing spaces
                while (*tok == ' ') tok++;
                char* end = tok + strlen(tok) - 1;
                while (end >= tok && (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t')) { *end = '\0'; end--; }
                if (*tok) {
                    if (!trie_search(file_trie, tok)) {
                        FileMetadata* m = create_file_metadata(tok, "system", ss_id, ss_info.ip, ss_info.client_port);
                        trie_insert(file_trie, tok, m);
                        cache_put(meta_cache, tok, m);
                        seeded++;
                    }
                }
                tok = strtok_r(NULL, ",", &saveptr);
            }
            if (seeded > 0) {
                trie_save_to_disk(file_trie, "metadata.dat");
            }
            pthread_mutex_unlock(&trie_mutex);
        }
        // Recovery: resync primary files from replicas (best-effort)
        // Gather all files and push content from any live replica to this SS
        char allfiles[1000][MAX_FILENAME_LEN];
        int total = trie_get_all_files(file_trie, allfiles, 1000);
        for (int i=0;i<total;i++) {
            FileMetadata* m = trie_search(file_trie, allfiles[i]); if (!m) continue;
            if (m->ss_id != ss_id) continue; // only files for which this SS is primary
            // pick alive replica
            int rid = replicas_pick_alive_for_access(m->filename, ss_id);
            if (rid < 0 || rid == ss_id) continue;
            // fetch from replica via client port
            char buf[MAX_CONTENT_LEN]; buf[0]='\0';
            // Temporarily use helper to fetch content
            // Duplicate of fetch_file_content_from_ss signature not visible here; inline minimal fetch
            StorageServerInfo *rss = get_storage_server(rid);
            if (!rss) continue;
            int fd = connect_to_server(rss->ip, rss->client_port);
            if (fd >= 0) {
                Message r={0}; r.type=MSG_READ_REQUEST; strcpy(r.filename, m->filename); send_message(fd,&r);
                Message rr; if (recv_message(fd,&rr)>0 && rr.type==MSG_ACK) { strncpy(buf, rr.data, sizeof(buf)-1); buf[sizeof(buf)-1]='\0'; }
                close_socket(fd);
            }
            if (buf[0]) {
                Message w={0}; w.type=MSG_WRITE_FILE_SS; strncpy(w.filename, m->filename, sizeof(w.filename)-1); strncpy(w.data, buf, sizeof(w.data)-1);
                send_message(ss_socket,&w);
                // do not wait for ACK
            }
        }
    }
    
    // Keep connection alive to receive updates
    while (1) {
        Message msg;
        int received = recv_message(ss_socket, &msg);
        
        if (received <= 0) break;
        
    if (msg.type == MSG_UPDATE_METADATA) {
            // Update file metadata: arg1=word_count, arg2=char_count
            pthread_mutex_lock(&trie_mutex);
            FileMetadata* metadata = trie_search(file_trie, msg.filename);
            if (metadata) {
                metadata->word_count = msg.arg1;
                metadata->char_count = msg.arg2;
                metadata->size_bytes = msg.arg2;
                metadata->last_modified = time(NULL);
                trie_save_to_disk(file_trie, "metadata.dat");
                cache_put(meta_cache, msg.filename, metadata);
            }
            pthread_mutex_unlock(&trie_mutex);
            
            // Kick off async replication of content to replicas
            async_replicate_file(msg.filename, metadata);
            Message ack = (Message){0};
            ack.type = MSG_ACK;
            send_message(ss_socket, &ack);
        }
    }
    
    return NULL;
}

// Helper: fetch content from a given SS id
static char *fetch_from_ss_id(int ss_id, const char *file) {
    StorageServerInfo *ss = get_storage_server(ss_id); if (!ss) return NULL;
    int fd = connect_to_server(ss->ip, ss->client_port); if (fd<0) return NULL;
    Message r={0}; r.type=MSG_READ_REQUEST; strncpy(r.filename,file,sizeof(r.filename)-1); send_message(fd,&r);
    Message rr; char *out=NULL; if (recv_message(fd,&rr)>0 && rr.type==MSG_ACK) { out=strdup(rr.data);} close_socket(fd); return out;
}

static void *replicate_thread(void *arg) {
    // arg is strdup'd filename
    char *fname = (char*)arg;
    // We need metadata; access under lock
    extern pthread_mutex_t trie_mutex; extern TrieNode* file_trie; // already defined
    pthread_mutex_lock(&trie_mutex);
    FileMetadata *m = trie_search(file_trie, fname);
    pthread_mutex_unlock(&trie_mutex);
    if (!m) { free(fname); return NULL; }
    // Fetch content from whichever server is currently serving primary (if primary dead, pick alive)
    int src_id = replicas_pick_alive_for_access(m->filename, m->ss_id);
    char *content = fetch_from_ss_id(src_id>=0?src_id:m->ss_id, m->filename);
    if (!content) { free(fname); return NULL; }
    int rids[MAX_STORAGE_SERVERS]; int n = replicas_get(m->filename, rids, MAX_STORAGE_SERVERS);
    for (int i=0;i<n;i++) {
        int rid = rids[i]; if (rid == src_id) continue;
        StorageServerInfo *rss = get_storage_server(rid); if (!rss || !rss->is_alive) continue;
        // Push content to replica via NM->SS control channel
        Message w={0}; w.type=MSG_WRITE_FILE_SS; strncpy(w.filename, m->filename, sizeof(w.filename)-1);
        size_t tocpy = (strlen(content) < sizeof(w.data)-1) ? strlen(content) : sizeof(w.data)-1; memcpy(w.data, content, tocpy); w.data[tocpy]='\0';
        send_message(rss->socket_fd, &w);
        // no ACK awaited (async)
    }
    free(content); free(fname); return NULL;
}

static void async_replicate_file(const char *filename, FileMetadata *metadata) {
    (void)metadata;
    char *fn = strdup(filename);
    pthread_t th; pthread_create(&th, NULL, replicate_thread, fn); pthread_detach(th);
}

int main(int argc, char** argv) {
    printf("🚀 Starting Naming Server...\n");
    // Prevent SIGPIPE from killing the process when a peer disconnects
    signal(SIGPIPE, SIG_IGN);
    int listen_port = NM_PORT;
    if (argc >= 2) {
        int p = atoi(argv[1]);
        if (p > 0 && p < 65536) listen_port = p;
    }
    
    // Initialize
    file_trie = trie_load_from_disk("metadata.dat");
    // Load persistent users
    extern void users_load_from_disk(const char* path); // forward from nm_storage.c
    users_load_from_disk("users.dat");
    meta_cache = cache_init(CACHE_CAPACITY);
    printf("📂 Loaded metadata from disk\n");
    init_logger("logs/nm_logs.txt", "NM");
    // Load folders (bonus)
    extern void folders_load_from_disk(const char* path);
    folders_load_from_disk("folders.dat");
    // Load access requests (bonus)
    extern void access_requests_load_from_disk(const char* path);
    access_requests_load_from_disk("access_requests.dat");
    
    // Start server
    int server_socket = start_server(listen_port, 50);
    if (server_socket < 0) {
        printf("❌ Failed to start server\n");
        return 1;
    }
    
    printf("✅ Naming Server running on port %d\n\n", listen_port);
    
    // Heartbeat thread for failure detection (simple)
    pthread_t hb;
    pthread_create(&hb, NULL, heartbeat_thread, NULL);

    while (1) {
        char client_ip[MAX_IP_LEN];
        int client_port;
        
        int client_socket = accept_connection(server_socket, client_ip, &client_port);
        if (client_socket < 0) continue;
        
        printf("📥 New connection from %s:%d\n", client_ip, client_port);
        
        // Peek at first message to determine if SS or client (ensure full struct is available)
        Message peek_msg;
        int got = recv(client_socket, &peek_msg, sizeof(Message), MSG_PEEK | MSG_WAITALL);
        
        pthread_t thread;
        ConnectionArgs* cargs = (ConnectionArgs*)malloc(sizeof(ConnectionArgs));
        cargs->socket_fd = client_socket;
        strncpy(cargs->ip, client_ip, sizeof(cargs->ip)-1);
        cargs->ip[sizeof(cargs->ip)-1] = '\0';
        cargs->port = client_port;
        if (got == (int)sizeof(Message) && peek_msg.type == MSG_SS_REGISTER) {
            pthread_create(&thread, NULL, handle_ss_thread, cargs);
        } else {
            pthread_create(&thread, NULL, handle_client_thread, cargs);
        }
        
        pthread_detach(thread);
    }
    
    close_logger();
    return 0;
}
