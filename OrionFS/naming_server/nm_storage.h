#ifndef NM_STORAGE_H
#define NM_STORAGE_H

#include "../common/protocol.h"

// ==================== TRIE OPERATIONS ====================

// Initialize Trie
TrieNode* trie_init();

// Insert file metadata into Trie
int trie_insert(TrieNode* root, const char* filename, FileMetadata* data);

// Search for file in Trie - O(m) where m = filename length
FileMetadata* trie_search(TrieNode* root, const char* filename);

// Delete file from Trie
int trie_delete(TrieNode* root, const char* filename);

// Get all files (for VIEW command)
int trie_get_all_files(TrieNode* root, char filenames[][MAX_FILENAME_LEN], int max_count);

// Free Trie
void trie_free(TrieNode* root);

// Save Trie to disk
int trie_save_to_disk(TrieNode* root, const char* filepath);

// Load Trie from disk
TrieNode* trie_load_from_disk(const char* filepath);

// ==================== LRU CACHE OPERATIONS ====================

// Initialize LRU Cache
LRUCache* cache_init(int capacity);

// Get from cache - O(1)
FileMetadata* cache_get(LRUCache* cache, const char* filename);

// Put into cache - O(1)
void cache_put(LRUCache* cache, const char* filename, FileMetadata* data);

// Remove from cache
void cache_remove(LRUCache* cache, const char* filename);

// Free cache
void cache_free(LRUCache* cache);

// ==================== METADATA MANAGEMENT ====================

// Create new file metadata
FileMetadata* create_file_metadata(const char* filename, const char* owner, 
                                   int ss_id, const char* ss_ip, int ss_client_port);

// Update file metadata
int update_file_metadata(FileMetadata* metadata);

// Add access to file
int add_file_access(FileMetadata* metadata, const char* username, AccessType access);

// Remove access from file
int remove_file_access(FileMetadata* metadata, const char* username);

// Check if user has access
AccessType check_file_access(FileMetadata* metadata, const char* username);

// Get files accessible by user
int get_user_accessible_files(TrieNode* root, const char* username, 
                               char filenames[][MAX_FILENAME_LEN], int max_count);

// ==================== PERSISTENCE ====================

// Save metadata to disk
int save_metadata_to_disk(TrieNode* root, const char* filepath);

// Load metadata from disk
int load_metadata_from_disk(TrieNode* root, const char* filepath);

// ==================== STORAGE SERVER MANAGEMENT ====================

// Storage Server registry (global in naming_server.c)
extern StorageServerInfo ss_registry[MAX_STORAGE_SERVERS];
extern int ss_count;
extern pthread_mutex_t ss_registry_mutex;

// Register new storage server
int register_storage_server(StorageServerInfo* ss_info);

// Get storage server by ID
StorageServerInfo* get_storage_server(int ss_id);

// Get storage server for file
StorageServerInfo* get_storage_server_for_file(const char* filename);

// Select storage server for new file (load balancing)
StorageServerInfo* select_storage_server_for_new_file();

// Mark storage server as alive/dead
void set_storage_server_status(int ss_id, bool is_alive);

// ==================== CLIENT MANAGEMENT ====================

// Client registry (global in naming_server.c)
extern ClientInfo client_registry[MAX_CLIENTS];
extern int client_count;
extern pthread_mutex_t client_registry_mutex;

// Register new client
int register_client(ClientInfo* client_info);

// Get client by socket
ClientInfo* get_client_by_socket(int socket_fd);

// Get client by username
ClientInfo* get_client_by_username(const char* username);

// Remove client
void remove_client(int socket_fd);

// Get all usernames
int get_all_usernames(char usernames[][MAX_USERNAME_LEN], int max_count);

// ==================== FOLDER MANAGEMENT (Bonus) ====================

// Create a new folder (no-op if exists)
int create_folder(const char* foldername);

// Move file into folder (removes from any previous folder, adds to target)
int move_file_to_folder(const char* filename, const char* foldername);

// List files in folder; returns count
int list_folder_files(const char* foldername, char out[][MAX_FILENAME_LEN], int max_count);

// Persistence for folders
void folders_load_from_disk(const char* path);
int folders_save_to_disk(const char* path);

// ==================== ACCESS REQUESTS (Bonus) ====================
// Load access requests from disk (filename|requester|requested_access|pending|request_time)
void access_requests_load_from_disk(const char* path);
// Append a new access request (persistent)
int access_requests_append(const char* filename, const char* requester, AccessType requested_access);
// List pending requests for an owner into buffer (one per line)
int access_requests_list_for_owner(const char* owner, char* out, size_t out_sz);
// Approve or deny a request; approve grants access
int access_requests_resolve(const char* filename, const char* requester, bool approve, AccessType grant_access);

// ==================== REPLICATION (Fault Tolerance) ====================

// Configure replicas for a file (primary id and replica ids)
int replicas_set(const char* filename, int primary_ss_id, const int *replica_ids, int replica_count);

// Get replica ids for a file; returns count (excludes primary)
int replicas_get(const char* filename, int *out_ids, int max_ids);

// Pick a replica server for new file (not equal to primary); returns ss_id or -1
int replicas_pick_alternate(int primary_ss_id);

// Find an alive server for serving read/write: primary if alive, else an alive replica; returns ss_id or -1
int replicas_pick_alive_for_access(const char* filename, int primary_ss_id);

// Mark SS status helpers
void set_storage_server_status(int ss_id, bool is_alive);
bool get_storage_server_status(int ss_id);


#endif // NM_STORAGE_H
