#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include <pthread.h>

// ==================== CONSTANTS ====================
#define MAX_FILENAME_LEN 256
#define MAX_USERNAME_LEN 64
#define MAX_IP_LEN 16
#define MAX_CONTENT_LEN 4096
#define MAX_USERS 100
#define MAX_STORAGE_SERVERS 10
#define MAX_CLIENTS 50
#define MAX_FILES_PER_SS 1000
#define MAX_SENTENCES 1000
#define MAX_WORDS_PER_SENTENCE 1000
#define CACHE_CAPACITY 100

// ==================== MESSAGE TYPES ====================
typedef enum {
    // Client -> NM
    MSG_CLIENT_REGISTER,
    MSG_VIEW_FILES,
    MSG_READ_FILE,
    MSG_CREATE_FILE,
    MSG_WRITE_FILE,
    MSG_DELETE_FILE,
    MSG_INFO_FILE,
    MSG_STREAM_FILE,
    MSG_LIST_USERS,
    MSG_ADDACCESS,
    MSG_REMACCESS,
    MSG_EXEC_FILE,
    MSG_UNDO_FILE,
    
    // NM -> SS
    MSG_CREATE_FILE_SS,
    MSG_DELETE_FILE_SS,
    MSG_GET_FILE_CONTENT,
    MSG_WRITE_FILE_SS,
    // Physical folder ops (Bonus)
    MSG_CREATE_FOLDER_SS,
    MSG_MOVE_FILE_SS,
    
    // SS -> NM
    MSG_SS_REGISTER,
    MSG_SS_ACK,
    MSG_SS_ERROR,
    MSG_SS_HEARTBEAT,
    MSG_UPDATE_METADATA,  // SS notifies NM to update file metadata
    
    // NM -> Client
    MSG_SS_INFO,          // IP, port for direct connection
    MSG_FILE_LIST,
    MSG_USER_LIST,
    MSG_FILE_INFO,
    MSG_ACK,
    MSG_ERROR,
    
    // Client -> SS (direct)
    MSG_READ_REQUEST,
    MSG_WRITE_REQUEST,
    MSG_STREAM_REQUEST,
    MSG_EXEC_READ_REQUEST, // NM -> SS fetch for EXEC (logged as EXEC on SS)
    // Exec streaming
    MSG_EXEC_OUTPUT,
    MSG_EXEC_DONE,
    
    // Bonus
    MSG_CREATE_FOLDER,
    MSG_MOVE_FILE,
    MSG_VIEW_FOLDER,
    MSG_CHECKPOINT,
    MSG_REQUEST_ACCESS
    ,MSG_CP_CREATE      // Client -> NM (checkpoint create) or Client -> SS
    ,MSG_CP_VIEW        // View checkpoint content
    ,MSG_CP_REVERT      // Revert file to checkpoint
    ,MSG_CP_LIST        // List checkpoints for a file
    ,MSG_VIEW_REQUESTS   // Owner views pending access requests
    ,MSG_APPROVE_REQUEST // Owner approves a pending request
    ,MSG_DENY_REQUEST    // Owner denies a pending request
} MessageType;

// ==================== ERROR CODES ====================
typedef enum {
    ERR_SUCCESS = 0,
    ERR_FILE_NOT_FOUND,
    ERR_ACCESS_DENIED,
    ERR_FILE_EXISTS,
    ERR_SENTENCE_LOCKED,
    ERR_INVALID_INDEX,
    ERR_SS_UNAVAILABLE,
    ERR_INVALID_COMMAND,
    ERR_PERMISSION_DENIED,
    ERR_NOT_OWNER,
    ERR_CONNECTION_FAILED,
    ERR_TIMEOUT,
    ERR_INVALID_SENTENCE,
    ERR_INVALID_WORD_INDEX,
    ERR_FILE_LOCKED,
    ERR_UNKNOWN,
    ERR_REQUEST_NOT_FOUND,
    ERR_REQUEST_ALREADY_EXISTS
} ErrorCode;

// ==================== ACCESS TYPES ====================
typedef enum {
    ACCESS_NONE = 0,
    ACCESS_READ = 1,
    ACCESS_WRITE = 2  // Write implies read
} AccessType;

// ==================== DATA STRUCTURES ====================

// Generic Message Structure
typedef struct {
    MessageType type;
    char sender[MAX_USERNAME_LEN];
    char filename[MAX_FILENAME_LEN];
    char data[MAX_CONTENT_LEN];
    int arg1;  // Multi-purpose (sentence_idx, port, etc.)
    int arg2;  // Multi-purpose (word_idx, etc.)
    ErrorCode error_code;
    time_t timestamp;
} Message;

// File Metadata (stored in NM)
typedef struct {
    char filename[MAX_FILENAME_LEN];
    char owner[MAX_USERNAME_LEN];
    int ss_id;  // Storage server ID
    char ss_ip[MAX_IP_LEN];
    int ss_client_port;
    
    // Access control: stores username and their access type
    struct {
        char username[MAX_USERNAME_LEN];
        AccessType access;
    } access_list[MAX_USERS];
    int access_count;
    
    // File statistics
    time_t created_at;
    time_t last_modified;
    time_t last_accessed;
    char last_accessed_by[MAX_USERNAME_LEN];
    size_t size_bytes;
    int word_count;
    int char_count;
} FileMetadata;

// Storage Server Info (stored in NM)
typedef struct {
    int id;
    char ip[MAX_IP_LEN];
    int nm_port;         // Port for NM communication
    int client_port;     // Port for client communication
    bool is_alive;
    char file_list[MAX_FILES_PER_SS][MAX_FILENAME_LEN];
    int file_count;
    time_t last_heartbeat;
    int socket_fd;
} StorageServerInfo;

// Client Info (stored in NM)
typedef struct {
    int socket_fd;
    char username[MAX_USERNAME_LEN];
    char ip[MAX_IP_LEN];
    int nm_port;
    int ss_port;
    int conn_port; // TCP connection source port (ephemeral) captured by NM
    bool is_connected;
    time_t connected_at;
} ClientInfo;

// Trie Node for Efficient Search
typedef struct TrieNode {
    struct TrieNode* children[256];  // ASCII characters
    FileMetadata* file_data;
    bool is_end_of_word;
} TrieNode;

// LRU Cache Node
typedef struct CacheNode {
    char filename[MAX_FILENAME_LEN];
    FileMetadata* data;
    struct CacheNode *prev, *next;
    time_t access_time;
} CacheNode;

// LRU Cache
typedef struct {
    CacheNode* head;
    CacheNode* tail;
    int capacity;
    int size;
} LRUCache;

// Sentence Lock (used in SS)
typedef struct {
    char filename[MAX_FILENAME_LEN];
    int sentence_idx;
    char locked_by[MAX_USERNAME_LEN];
    bool is_locked;
    pthread_mutex_t mutex;
    time_t lock_time;
} SentenceLock;

// Undo Entry (used in SS)
typedef struct {
    char filename[MAX_FILENAME_LEN];
    char* previous_content;
    time_t timestamp;
} UndoEntry;

// ==================== BONUS STRUCTURES ====================

// Folder Node (Hierarchical structure)
typedef struct FolderNode {
    char name[MAX_FILENAME_LEN];
    bool is_folder;
    struct FolderNode* parent;
    struct FolderNode** children;
    int child_count;
    FileMetadata* file_data;  // NULL if folder
} FolderNode;

// Checkpoint
typedef struct {
    char tag[MAX_FILENAME_LEN];
    char* content_snapshot;
    time_t timestamp;
} Checkpoint;

// Access Request
typedef struct {
    char filename[MAX_FILENAME_LEN];
    char requester[MAX_USERNAME_LEN];
    AccessType requested_access;
    bool pending;
    time_t request_time;
} AccessRequest;

// Replica Info (Fault Tolerance)
typedef struct {
    char filename[MAX_FILENAME_LEN];
    int primary_ss_id;
    int replica_ss_ids[MAX_STORAGE_SERVERS];
    int replica_count;
} ReplicaInfo;

// ==================== FUNCTION PROTOTYPES ====================

// Error code to string conversion
const char* error_to_string(ErrorCode err);

// Message serialization/deserialization
int serialize_message(Message* msg, char* buffer, int buffer_size);
int deserialize_message(char* buffer, Message* msg);

#endif // PROTOCOL_H
