#include "nm_storage.h"
#include <stdlib.h>
#include <string.h>

// Global registries
StorageServerInfo ss_registry[MAX_STORAGE_SERVERS];
int ss_count = 0;
pthread_mutex_t ss_registry_mutex = PTHREAD_MUTEX_INITIALIZER;

ClientInfo client_registry[MAX_CLIENTS];
int client_count = 0;
pthread_mutex_t client_registry_mutex = PTHREAD_MUTEX_INITIALIZER;

// Persistent user list (union of all ever-registered usernames)
static char persistent_users[MAX_CLIENTS][MAX_USERNAME_LEN];
static int persistent_user_count = 0;

void users_load_from_disk(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!*line) continue;
        if (persistent_user_count < MAX_CLIENTS) {
            strncpy(persistent_users[persistent_user_count++], line, MAX_USERNAME_LEN - 1);
        }
    }
    fclose(f);
}

static void users_save_append(const char* username, const char* path) {
    // Avoid duplicates in persistent list
    for (int i = 0; i < persistent_user_count; ++i) {
        if (strcmp(persistent_users[i], username) == 0) return;
    }
    if (persistent_user_count < MAX_CLIENTS) {
        strncpy(persistent_users[persistent_user_count], username, MAX_USERNAME_LEN - 1);
        persistent_user_count++;
        FILE* f = fopen(path, "a");
        if (f) {
            fprintf(f, "%s\n", username);
            fclose(f);
        }
    }
}

// ==================== TRIE OPERATIONS ====================

TrieNode* trie_init() {
    TrieNode* root = (TrieNode*)calloc(1, sizeof(TrieNode));
    return root;
}

int trie_insert(TrieNode* root, const char* filename, FileMetadata* data) {
    if (!root || !filename || !data) return -1;
    
    TrieNode* current = root;
    for (int i = 0; filename[i] != '\0'; i++) {
        unsigned char ch = (unsigned char)filename[i];
        if (current->children[ch] == NULL) {
            current->children[ch] = (TrieNode*)calloc(1, sizeof(TrieNode));
        }
        current = current->children[ch];
    }
    
    current->is_end_of_word = true;
    current->file_data = data;
    return 0;
}

FileMetadata* trie_search(TrieNode* root, const char* filename) {
    if (!root || !filename) return NULL;
    
    TrieNode* current = root;
    for (int i = 0; filename[i] != '\0'; i++) {
        unsigned char ch = (unsigned char)filename[i];
        if (current->children[ch] == NULL) {
            return NULL;
        }
        current = current->children[ch];
    }
    
    return (current->is_end_of_word) ? current->file_data : NULL;
}

int trie_delete(TrieNode* root, const char* filename) {
    FileMetadata* data = trie_search(root, filename);
    if (data) {
        free(data);
    }
    // Simple approach: just mark as deleted
    TrieNode* current = root;
    for (int i = 0; filename[i] != '\0'; i++) {
        unsigned char ch = (unsigned char)filename[i];
        if (current->children[ch] == NULL) return -1;
        current = current->children[ch];
    }
    current->is_end_of_word = false;
    current->file_data = NULL;
    return 0;
}

void trie_get_all_helper(TrieNode* node, char* prefix, int depth, 
                         char filenames[][MAX_FILENAME_LEN], int* count, int max_count) {
    if (*count >= max_count) return;
    
    if (node->is_end_of_word && node->file_data != NULL) {
        prefix[depth] = '\0';
        strcpy(filenames[*count], prefix);
        (*count)++;
    }
    
    for (int i = 0; i < 256; i++) {
        if (node->children[i] != NULL) {
            prefix[depth] = (char)i;
            trie_get_all_helper(node->children[i], prefix, depth + 1, filenames, count, max_count);
        }
    }
}

int trie_get_all_files(TrieNode* root, char filenames[][MAX_FILENAME_LEN], int max_count) {
    char prefix[MAX_FILENAME_LEN];
    int count = 0;
    trie_get_all_helper(root, prefix, 0, filenames, &count, max_count);
    return count;
}

void trie_free(TrieNode* root) {
    if (!root) return;
    for (int i = 0; i < 256; i++) {
        if (root->children[i]) {
            trie_free(root->children[i]);
        }
    }
    if (root->file_data) free(root->file_data);
    free(root);
}

// Save Trie to disk (extended format)
// Format per line:
// filename|owner|ss_id|ss_ip|ss_client_port|created_at|last_modified|last_accessed|last_accessed_by|size_bytes|word_count|char_count|access_count|user1:access,user2:access
void trie_save_helper(TrieNode* node, FILE* fp, char* prefix, int depth) {
    if (!node) return;
    
    if (node->is_end_of_word && node->file_data) {
        prefix[depth] = '\0';
    FileMetadata* m = node->file_data;
    fprintf(fp, "%s|%s|%d|%s|%d|%ld|%ld|%ld|%s|%zu|%d|%d|%d|",
        prefix,
        m->owner,
        m->ss_id,
        m->ss_ip,
        m->ss_client_port,
        (long)m->created_at,
        (long)m->last_modified,
        (long)m->last_accessed,
        m->last_accessed_by,
        m->size_bytes,
        m->word_count,
        m->char_count,
        m->access_count);
    for (int i = 0; i < m->access_count; i++) {
        fprintf(fp, "%s:%d%s",
            m->access_list[i].username,
            (int)m->access_list[i].access,
            (i < m->access_count - 1) ? "," : "");
    }
    fprintf(fp, "\n");
    }
    
    for (int i = 0; i < 256; i++) {
        if (node->children[i]) {
            prefix[depth] = (char)i;
            trie_save_helper(node->children[i], fp, prefix, depth + 1);
        }
    }
}

int trie_save_to_disk(TrieNode* root, const char* filepath) {
    FILE* fp = fopen(filepath, "w");
    if (!fp) return -1;
    
    char prefix[MAX_FILENAME_LEN];
    trie_save_helper(root, fp, prefix, 0);
    fclose(fp);
    return 0;
}

// Load Trie from disk
TrieNode* trie_load_from_disk(const char* filepath) {
    FILE* fp = fopen(filepath, "r");
    if (!fp) return trie_init(); // Return empty trie if file doesn't exist
    
    TrieNode* root = trie_init();
    char line[4096];
    
    while (fgets(line, sizeof(line), fp)) {
        // Trim trailing newline
        size_t len = strlen(line);
        if (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';

        // Tokenize by '|'
        char* tokens[14];
        int ntok = 0;
        char* saveptr = NULL;
        char* tok = strtok_r(line, "|", &saveptr);
        while (tok && ntok < 14) {
            tokens[ntok++] = tok;
            tok = strtok_r(NULL, "|", &saveptr);
        }

        if (ntok >= 14) {
            // Extended format
            const char* filename = tokens[0];
            const char* owner = tokens[1];
            int ss_id = atoi(tokens[2]);
            const char* ss_ip = tokens[3];
            int ss_port = atoi(tokens[4]);
            time_t created_at = (time_t)atol(tokens[5]);
            time_t last_modified = (time_t)atol(tokens[6]);
            time_t last_accessed = (time_t)atol(tokens[7]);
            const char* last_accessed_by = tokens[8];
            size_t size_bytes = (size_t)strtoull(tokens[9], NULL, 10);
            int word_count = atoi(tokens[10]);
            int char_count = atoi(tokens[11]);
            // tokens[12] is legacy access_count field from older format; we recompute from ACL list
            /* int legacy_access_count = atoi(tokens[12]); */
            const char* acl_str = tokens[13];

            FileMetadata* m = (FileMetadata*)calloc(1, sizeof(FileMetadata));
            strncpy(m->filename, filename, MAX_FILENAME_LEN - 1);
            strncpy(m->owner, owner, MAX_USERNAME_LEN - 1);
            m->ss_id = ss_id;
            strncpy(m->ss_ip, ss_ip, MAX_IP_LEN - 1);
            m->ss_client_port = ss_port;
            m->created_at = created_at;
            m->last_modified = last_modified;
            m->last_accessed = last_accessed;
            strncpy(m->last_accessed_by, last_accessed_by, MAX_USERNAME_LEN - 1);
            m->size_bytes = size_bytes;
            m->word_count = word_count;
            m->char_count = char_count;
            m->access_count = 0;

            // Parse ACL list: user:access,user2:access
            if (acl_str && *acl_str) {
                char acl_buf[2048];
                strncpy(acl_buf, acl_str, sizeof(acl_buf)-1);
                acl_buf[sizeof(acl_buf)-1] = '\0';
                char* save2 = NULL;
                char* pair = strtok_r(acl_buf, ",", &save2);
                while (pair && m->access_count < MAX_USERS) {
                    char* sep = strchr(pair, ':');
                    if (sep) {
                        *sep = '\0';
                        const char* u = pair;
                        int acc = atoi(sep + 1);
                        strncpy(m->access_list[m->access_count].username, u, MAX_USERNAME_LEN - 1);
                        m->access_list[m->access_count].access = (AccessType)acc;
                        m->access_count++;
                    }
                    pair = strtok_r(NULL, ",", &save2);
                }
            }

            // Ensure owner has at least WRITE access
            bool owner_present = false;
            for (int i = 0; i < m->access_count; i++) {
                if (strcmp(m->access_list[i].username, m->owner) == 0) {
                    if (m->access_list[i].access != ACCESS_WRITE)
                        m->access_list[i].access = ACCESS_WRITE;
                    owner_present = true;
                    break;
                }
            }
            if (!owner_present && m->access_count < MAX_USERS) {
                strncpy(m->access_list[m->access_count].username, m->owner, MAX_USERNAME_LEN - 1);
                m->access_list[m->access_count].access = ACCESS_WRITE;
                m->access_count++;
            }

            trie_insert(root, m->filename, m);
        } else if (ntok == 5) {
            // Legacy format
            const char* filename = tokens[0];
            const char* owner = tokens[1];
            int ss_id = atoi(tokens[2]);
            const char* ss_ip = tokens[3];
            int ss_port = atoi(tokens[4]);
            FileMetadata* metadata = create_file_metadata(filename, owner, ss_id, ss_ip, ss_port);
            trie_insert(root, filename, metadata);
        } else {
            // Skip invalid line
            continue;
        }
    }
    
    fclose(fp);
    return root;
}

// ==================== METADATA MANAGEMENT ====================

FileMetadata* create_file_metadata(const char* filename, const char* owner, 
                                   int ss_id, const char* ss_ip, int ss_client_port) {
    FileMetadata* metadata = (FileMetadata*)calloc(1, sizeof(FileMetadata));
    strncpy(metadata->filename, filename, MAX_FILENAME_LEN - 1);
    strncpy(metadata->owner, owner, MAX_USERNAME_LEN - 1);
    metadata->ss_id = ss_id;
    strncpy(metadata->ss_ip, ss_ip, MAX_IP_LEN - 1);
    metadata->ss_client_port = ss_client_port;
    
    // Owner always has RW access
    strncpy(metadata->access_list[0].username, owner, MAX_USERNAME_LEN - 1);
    metadata->access_list[0].access = ACCESS_WRITE;
    metadata->access_count = 1;
    
    metadata->created_at = time(NULL);
    metadata->last_modified = time(NULL);
    metadata->last_accessed = time(NULL);
    strncpy(metadata->last_accessed_by, owner, MAX_USERNAME_LEN - 1);
    
    return metadata;
}

int add_file_access(FileMetadata* metadata, const char* username, AccessType access) {
    if (!metadata || !username) return -1;
    
    // Owner always has RW; don't modify owner entry
    if (strcmp(metadata->owner, username) == 0) {
        // Ensure it stays WRITE
        for (int i = 0; i < metadata->access_count; i++) {
            if (strcmp(metadata->access_list[i].username, username) == 0) {
                metadata->access_list[i].access = ACCESS_WRITE;
                return 0;
            }
        }
        // If missing (shouldn't happen), add as WRITE
        if (metadata->access_count < MAX_USERS) {
            strncpy(metadata->access_list[metadata->access_count].username, username, MAX_USERNAME_LEN - 1);
            metadata->access_list[metadata->access_count].access = ACCESS_WRITE;
            metadata->access_count++;
            return 0;
        }
        return -1;
    }

    // Check if user already has access
    for (int i = 0; i < metadata->access_count; i++) {
        if (strcmp(metadata->access_list[i].username, username) == 0) {
            // Never downgrade WRITE to READ
            if (metadata->access_list[i].access == ACCESS_WRITE) return 0;
            metadata->access_list[i].access = access;
            return 0;
        }
    }
    
    // Add new access
    if (metadata->access_count < MAX_USERS) {
        strncpy(metadata->access_list[metadata->access_count].username, username, MAX_USERNAME_LEN - 1);
        metadata->access_list[metadata->access_count].access = access;
        metadata->access_count++;
        return 0;
    }
    
    return -1;
}

int remove_file_access(FileMetadata* metadata, const char* username) {
    if (!metadata || !username) return -1;
    
    // Don't remove owner's access
    if (strcmp(metadata->owner, username) == 0) return -1;
    
    for (int i = 0; i < metadata->access_count; i++) {
        if (strcmp(metadata->access_list[i].username, username) == 0) {
            // Shift remaining entries
            for (int j = i; j < metadata->access_count - 1; j++) {
                metadata->access_list[j] = metadata->access_list[j + 1];
            }
            metadata->access_count--;
            return 0;
        }
    }
    
    return -1;
}

AccessType check_file_access(FileMetadata* metadata, const char* username) {
    if (!metadata || !username) return ACCESS_NONE;
    
    for (int i = 0; i < metadata->access_count; i++) {
        if (strcmp(metadata->access_list[i].username, username) == 0) {
            return metadata->access_list[i].access;
        }
    }
    
    return ACCESS_NONE;
}

int get_user_accessible_files(TrieNode* root, const char* username, 
                               char filenames[][MAX_FILENAME_LEN], int max_count) {
    char all_files[1000][MAX_FILENAME_LEN];
    int total = trie_get_all_files(root, all_files, 1000);
    int count = 0;
    
    for (int i = 0; i < total && count < max_count; i++) {
        FileMetadata* meta = trie_search(root, all_files[i]);
        if (meta && check_file_access(meta, username) != ACCESS_NONE) {
            strcpy(filenames[count++], all_files[i]);
        }
    }
    
    return count;
}

// ==================== LRU CACHE (HashMap + Doubly Linked List) ====================

// Simple hash entry for filename -> CacheNode*
typedef struct HashEntry {
    char key[MAX_FILENAME_LEN];
    CacheNode* node;
    struct HashEntry* next;
} HashEntry;

typedef struct HashTable {
    HashEntry** buckets;
    int capacity; // number of buckets
} HashTable;

static unsigned long hash_filename(const char* str) {
    // djb2
    unsigned long hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + (unsigned long)c;
    }
    return hash;
}

static HashTable* ht_create(int capacity) {
    HashTable* ht = (HashTable*)calloc(1, sizeof(HashTable));
    ht->capacity = capacity;
    ht->buckets = (HashEntry**)calloc(capacity, sizeof(HashEntry*));
    return ht;
}

static CacheNode* ht_get(HashTable* ht, const char* key) {
    unsigned long h = hash_filename(key) % ht->capacity;
    HashEntry* e = ht->buckets[h];
    while (e) {
        if (strcmp(e->key, key) == 0) return e->node;
        e = e->next;
    }
    return NULL;
}

static void ht_put(HashTable* ht, const char* key, CacheNode* node) {
    unsigned long h = hash_filename(key) % ht->capacity;
    HashEntry* e = ht->buckets[h];
    while (e) {
        if (strcmp(e->key, key) == 0) {
            e->node = node;
            return;
        }
        e = e->next;
    }
    // insert new at head
    HashEntry* ne = (HashEntry*)calloc(1, sizeof(HashEntry));
    strncpy(ne->key, key, MAX_FILENAME_LEN - 1);
    ne->node = node;
    ne->next = ht->buckets[h];
    ht->buckets[h] = ne;
}

static void ht_remove(HashTable* ht, const char* key) {
    unsigned long h = hash_filename(key) % ht->capacity;
    HashEntry* e = ht->buckets[h];
    HashEntry* prev = NULL;
    while (e) {
        if (strcmp(e->key, key) == 0) {
            if (prev) prev->next = e->next; else ht->buckets[h] = e->next;
            free(e);
            return;
        }
        prev = e;
        e = e->next;
    }
}

static void ht_free(HashTable* ht) {
    if (!ht) return;
    for (int i = 0; i < ht->capacity; ++i) {
        HashEntry* e = ht->buckets[i];
        while (e) {
            HashEntry* nxt = e->next;
            free(e);
            e = nxt;
        }
    }
    free(ht->buckets);
    free(ht);
}

// Extend LRUCache with map
typedef struct LRUImpl {
    LRUCache pub; // head, tail, capacity, size
    HashTable* map;
} LRUImpl;

static LRUImpl* as_impl(LRUCache* c) { return (LRUImpl*)c; }

static void detach_node(LRUCache* cache, CacheNode* node) {
    if (!node) return;
    if (node->prev) node->prev->next = node->next;
    if (node->next) node->next->prev = node->prev;
    if (cache->head == node) cache->head = node->next;
    if (cache->tail == node) cache->tail = node->prev;
    node->prev = node->next = NULL;
}

static void attach_front(LRUCache* cache, CacheNode* node) {
    node->prev = NULL;
    node->next = cache->head;
    if (cache->head) cache->head->prev = node;
    cache->head = node;
    if (!cache->tail) cache->tail = node;
}

LRUCache* cache_init(int capacity) {
    if (capacity <= 0) capacity = CACHE_CAPACITY;
    LRUImpl* impl = (LRUImpl*)calloc(1, sizeof(LRUImpl));
    impl->pub.capacity = capacity;
    impl->pub.size = 0;
    impl->pub.head = impl->pub.tail = NULL;
    // number of buckets: next power of two >= 2*capacity
    int buckets = 1;
    while (buckets < capacity * 2) buckets <<= 1;
    impl->map = ht_create(buckets);
    return (LRUCache*)impl;
}

FileMetadata* cache_get(LRUCache* cache, const char* filename) {
    if (!cache || !filename) return NULL;
    LRUImpl* impl = as_impl(cache);
    CacheNode* node = ht_get(impl->map, filename);
    if (!node) return NULL;
    node->access_time = time(NULL);
    // move to front
    detach_node(cache, node);
    attach_front(cache, node);
    return node->data;
}

void cache_put(LRUCache* cache, const char* filename, FileMetadata* data) {
    if (!cache || !filename || !data) return;
    LRUImpl* impl = as_impl(cache);
    CacheNode* node = ht_get(impl->map, filename);
    if (node) {
        node->data = data;
        node->access_time = time(NULL);
        detach_node(cache, node);
        attach_front(cache, node);
        return;
    }
    // New entry
    node = (CacheNode*)calloc(1, sizeof(CacheNode));
    strncpy(node->filename, filename, MAX_FILENAME_LEN - 1);
    node->data = data;
    node->access_time = time(NULL);
    attach_front(cache, node);
    ht_put(impl->map, filename, node);
    cache->size++;
    // Evict if needed
    if (cache->size > cache->capacity && cache->tail) {
        CacheNode* ev = cache->tail;
        detach_node(cache, ev);
        ht_remove(impl->map, ev->filename);
        free(ev);
        cache->size--;
    }
}

void cache_remove(LRUCache* cache, const char* filename) {
    if (!cache || !filename) return;
    LRUImpl* impl = as_impl(cache);
    CacheNode* node = ht_get(impl->map, filename);
    if (!node) return;
    detach_node(cache, node);
    ht_remove(impl->map, filename);
    free(node);
    if (cache->size > 0) cache->size--;
}

void cache_free(LRUCache* cache) {
    if (!cache) return;
    LRUImpl* impl = as_impl(cache);
    CacheNode* cur = cache->head;
    while (cur) {
        CacheNode* nxt = cur->next;
        free(cur);
        cur = nxt;
    }
    ht_free(impl->map);
    free(impl);
}

// ==================== STORAGE SERVER MANAGEMENT ====================

int register_storage_server(StorageServerInfo* ss_info) {
    pthread_mutex_lock(&ss_registry_mutex);
    
    // Try to match existing by IP + client_port to support recovery reuse
    for (int i=0; i<ss_count; ++i) {
        if (strcmp(ss_registry[i].ip, ss_info->ip)==0 && ss_registry[i].client_port == ss_info->client_port) {
            ss_registry[i].nm_port = ss_info->nm_port;
            ss_registry[i].is_alive = ss_info->is_alive;
            ss_registry[i].last_heartbeat = time(NULL);
            ss_registry[i].socket_fd = ss_info->socket_fd;
            int id = ss_registry[i].id;
            pthread_mutex_unlock(&ss_registry_mutex);
            return id;
        }
    }
    
    if (ss_count >= MAX_STORAGE_SERVERS) {
        pthread_mutex_unlock(&ss_registry_mutex);
        return -1;
    }
    
    ss_registry[ss_count] = *ss_info;
    ss_registry[ss_count].id = ss_count;
    ss_registry[ss_count].last_heartbeat = time(NULL);
    ss_count++;
    
    pthread_mutex_unlock(&ss_registry_mutex);
    return ss_count - 1;
}

StorageServerInfo* get_storage_server(int ss_id) {
    if (ss_id < 0 || ss_id >= ss_count) return NULL;
    return &ss_registry[ss_id];
}

StorageServerInfo* select_storage_server_for_new_file() {
    if (ss_count == 0) return NULL;
    
    // Simple round-robin
    static int last_selected = -1;
    last_selected = (last_selected + 1) % ss_count;
    
    return &ss_registry[last_selected];
}

// ==================== CLIENT MANAGEMENT ====================

int register_client(ClientInfo* client_info) {
    pthread_mutex_lock(&client_registry_mutex);
    
    if (client_count >= MAX_CLIENTS) {
        pthread_mutex_unlock(&client_registry_mutex);
        return -1;
    }
    
    client_registry[client_count] = *client_info;
    client_count++;
    users_save_append(client_info->username, "users.dat");
    
    pthread_mutex_unlock(&client_registry_mutex);
    return client_count - 1;
}

ClientInfo* get_client_by_socket(int socket_fd) {
    for (int i = 0; i < client_count; i++) {
        if (client_registry[i].socket_fd == socket_fd) {
            return &client_registry[i];
        }
    }
    return NULL;
}

ClientInfo* get_client_by_username(const char* username) {
    for (int i = 0; i < client_count; i++) {
        if (strcmp(client_registry[i].username, username) == 0) {
            return &client_registry[i];
        }
    }
    return NULL;
}

void remove_client(int socket_fd) {
    pthread_mutex_lock(&client_registry_mutex);
    
    for (int i = 0; i < client_count; i++) {
        if (client_registry[i].socket_fd == socket_fd) {
            for (int j = i; j < client_count - 1; j++) {
                client_registry[j] = client_registry[j + 1];
            }
            client_count--;
            break;
        }
    }
    
    pthread_mutex_unlock(&client_registry_mutex);
}

int get_all_usernames(char usernames[][MAX_USERNAME_LEN], int max_count) {
    // Merge persistent users and active clients (dedup)
    int count = 0;
    // Add persistent
    for (int i = 0; i < persistent_user_count && count < max_count; ++i) {
        bool dup = false;
        for (int j = 0; j < count; ++j) {
            if (strcmp(usernames[j], persistent_users[i]) == 0) { dup = true; break; }
        }
        if (!dup) strcpy(usernames[count++], persistent_users[i]);
    }
    // Add currently active (ensure dedup)
    for (int i = 0; i < client_count && count < max_count; ++i) {
        bool dup = false;
        for (int j = 0; j < count; ++j) {
            if (strcmp(usernames[j], client_registry[i].username) == 0) { dup = true; break; }
        }
        if (!dup) strcpy(usernames[count++], client_registry[i].username);
    }
    return count;
}

// ==================== FOLDER MANAGEMENT (Bonus) ====================

typedef struct FolderEntry {
    char name[MAX_FILENAME_LEN];
    char files[1000][MAX_FILENAME_LEN];
    int file_count;
} FolderEntry;

static FolderEntry folders[100];
static int folder_count = 0;
static pthread_mutex_t folders_mutex = PTHREAD_MUTEX_INITIALIZER;

static int find_folder_index(const char* foldername) {
    for (int i = 0; i < folder_count; ++i) {
        if (strcmp(folders[i].name, foldername) == 0) return i;
    }
    return -1;
}

int create_folder(const char* foldername) {
    if (!foldername || !*foldername) return -1;
    pthread_mutex_lock(&folders_mutex);
    int idx = find_folder_index(foldername);
    if (idx >= 0) {
        pthread_mutex_unlock(&folders_mutex);
        return 0; // already exists
    }
    if (folder_count >= (int)(sizeof(folders)/sizeof(folders[0]))) {
        pthread_mutex_unlock(&folders_mutex);
        return -1;
    }
    FolderEntry* fe = &folders[folder_count++];
    strncpy(fe->name, foldername, MAX_FILENAME_LEN - 1);
    fe->file_count = 0;
    pthread_mutex_unlock(&folders_mutex);
    return 0;
}

// ==================== ACCESS REQUESTS (Bonus) ====================
typedef struct AccessRequestEntry {
    char filename[MAX_FILENAME_LEN];
    char requester[MAX_USERNAME_LEN];
    AccessType requested_access;
    bool pending;
    time_t request_time;
} AccessRequestEntry;

static AccessRequestEntry access_requests[1000];
static int access_request_count = 0;
static pthread_mutex_t access_requests_mutex = PTHREAD_MUTEX_INITIALIZER;

void access_requests_load_from_disk(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!*line) continue;
        char* tok; char* save=NULL; int field=0;
        char filename[MAX_FILENAME_LEN]=""; char requester[MAX_USERNAME_LEN]=""; int acc=0; int pend=0; long ts=0;
        tok = strtok_r(line, "|", &save);
        while (tok) {
            switch(field) {
                case 0: strncpy(filename, tok, sizeof(filename)-1); break;
                case 1: strncpy(requester, tok, sizeof(requester)-1); break;
                case 2: acc = atoi(tok); break;
                case 3: pend = atoi(tok); break;
                case 4: ts = atol(tok); break;
            }
            field++; tok = strtok_r(NULL, "|", &save);
        }
        if (field >= 5 && access_request_count < (int)(sizeof(access_requests)/sizeof(access_requests[0]))) {
            AccessRequestEntry* e = &access_requests[access_request_count++];
            strncpy(e->filename, filename, sizeof(e->filename)-1);
            strncpy(e->requester, requester, sizeof(e->requester)-1);
            e->requested_access = (AccessType)acc;
            e->pending = pend ? true : false;
            e->request_time = (time_t)ts;
        }
    }
    fclose(f);
}

static int access_requests_flush_all(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return -1;
    for (int i=0;i<access_request_count;i++) {
        AccessRequestEntry* e = &access_requests[i];
        fprintf(f, "%s|%s|%d|%d|%ld\n", e->filename, e->requester, (int)e->requested_access, e->pending?1:0, (long)e->request_time);
    }
    fclose(f); return 0;
}

int access_requests_append(const char* filename, const char* requester, AccessType requested_access) {
    if (!filename || !*filename || !requester || !*requester) return -1;
    pthread_mutex_lock(&access_requests_mutex);
    // Dedup: if existing pending identical request, reject
    for (int i=0;i<access_request_count;i++) {
        AccessRequestEntry* e=&access_requests[i];
        if (e->pending && strcmp(e->filename, filename)==0 && strcmp(e->requester, requester)==0) {
            pthread_mutex_unlock(&access_requests_mutex);
            return -2; // already exists
        }
    }
    if (access_request_count >= (int)(sizeof(access_requests)/sizeof(access_requests[0]))) {
        pthread_mutex_unlock(&access_requests_mutex);
        return -3; // full
    }
    AccessRequestEntry* e = &access_requests[access_request_count++];
    strncpy(e->filename, filename, sizeof(e->filename)-1);
    strncpy(e->requester, requester, sizeof(e->requester)-1);
    e->requested_access = requested_access;
    e->pending = true;
    e->request_time = time(NULL);
    access_requests_flush_all("access_requests.dat");
    pthread_mutex_unlock(&access_requests_mutex);
    return 0;
}

// Helper to format timestamps (YYYY-MM-DD HH:MM:SS)
static void timestamp_to_string(time_t t, char* buf, size_t sz) {
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, sz, "%Y-%m-%d %H:%M:%S", &tm);
}

// Actual implementation; requires external trie root & mutex declared in naming_server.c
extern TrieNode* file_trie; extern pthread_mutex_t trie_mutex;
int access_requests_list_for_owner(const char* owner, char* out, size_t out_sz) {
    if (!owner || !out) return 0;
    out[0]='\0';
    pthread_mutex_lock(&access_requests_mutex);
    pthread_mutex_lock(&trie_mutex);
    int count=0;
    for (int i=0;i<access_request_count;i++) {
        AccessRequestEntry* e=&access_requests[i];
        if (!e->pending) continue;
        FileMetadata* m = trie_search(file_trie, e->filename);
        if (!m) continue;
        if (strcmp(m->owner, owner)!=0) continue;
        char line[256];
        char tsbuf[64]; timestamp_to_string(e->request_time, tsbuf, sizeof(tsbuf));
        snprintf(line, sizeof(line), "--> %s | %s | %s | %s\n", e->filename, e->requester, e->requested_access==ACCESS_WRITE?"WRITE":"READ", tsbuf);
        if (strlen(out)+strlen(line) < out_sz-1) strcat(out, line);
        count++;
    }
    pthread_mutex_unlock(&trie_mutex);
    pthread_mutex_unlock(&access_requests_mutex);
    return count;
}

int access_requests_resolve(const char* filename, const char* requester, bool approve, AccessType grant_access) {
    pthread_mutex_lock(&access_requests_mutex);
    int result = -1;
    for (int i=0;i<access_request_count;i++) {
        AccessRequestEntry* e=&access_requests[i];
        if (e->pending && strcmp(e->filename, filename)==0 && strcmp(e->requester, requester)==0) {
            e->pending = false;
            result = 0;
            break;
        }
    }
    if (result == 0) access_requests_flush_all("access_requests.dat");
    pthread_mutex_unlock(&access_requests_mutex);
    if (result != 0) return -1;
    if (approve) {
        // Grant access in metadata
        pthread_mutex_lock(&trie_mutex);
        FileMetadata* m = trie_search(file_trie, filename);
        if (m) {
            add_file_access(m, requester, grant_access);
            trie_save_to_disk(file_trie, "metadata.dat");
        }
        pthread_mutex_unlock(&trie_mutex);
    }
    return 0;
}

int move_file_to_folder(const char* filename, const char* foldername) {
    if (!filename || !*filename || !foldername || !*foldername) return -1;
    pthread_mutex_lock(&folders_mutex);
    // Remove file from any existing folder
    for (int i = 0; i < folder_count; ++i) {
        for (int j = 0; j < folders[i].file_count; ++j) {
            if (strcmp(folders[i].files[j], filename) == 0) {
                // shift left
                for (int k = j; k < folders[i].file_count - 1; ++k) {
                    strcpy(folders[i].files[k], folders[i].files[k+1]);
                }
                folders[i].file_count--;
                break;
            }
        }
    }
    // Ensure target folder exists
    int idx = find_folder_index(foldername);
    if (idx < 0) {
        if (create_folder(foldername) != 0) { pthread_mutex_unlock(&folders_mutex); return -1; }
        idx = find_folder_index(foldername);
    }
    // Add if not present
    FolderEntry* fe = &folders[idx];
    for (int j = 0; j < fe->file_count; ++j) {
        if (strcmp(fe->files[j], filename) == 0) {
            pthread_mutex_unlock(&folders_mutex);
            return 0; // already present
        }
    }
    if (fe->file_count < 1000) {
        strncpy(fe->files[fe->file_count++], filename, MAX_FILENAME_LEN - 1);
        pthread_mutex_unlock(&folders_mutex);
        return 0;
    }
    pthread_mutex_unlock(&folders_mutex);
    return -1;
}

int list_folder_files(const char* foldername, char out[][MAX_FILENAME_LEN], int max_count) {
    pthread_mutex_lock(&folders_mutex);
    int idx = find_folder_index(foldername);
    int count = 0;
    if (idx >= 0) {
        for (int i = 0; i < folders[idx].file_count && count < max_count; ++i) {
            strcpy(out[count++], folders[idx].files[i]);
        }
    }
    pthread_mutex_unlock(&folders_mutex);
    return count;
}

void folders_load_from_disk(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!*line) continue;
        // format: folder|file1,file2,...
        char* sep = strchr(line, '|');
        if (!sep) continue;
        *sep = '\0';
        char* fname = line;
        char* flist = sep + 1;
        create_folder(fname);
        int idx = find_folder_index(fname);
        if (idx < 0) continue;
        if (*flist) {
            char buf[4096];
            strncpy(buf, flist, sizeof(buf)-1); buf[sizeof(buf)-1] = '\0';
            char* save = NULL;
            char* tok = strtok_r(buf, ",", &save);
            while (tok && folders[idx].file_count < 1000) {
                while (*tok == ' ') tok++;
                char* end = tok + strlen(tok) - 1;
                while (end >= tok && (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t')) { *end = '\0'; end--; }
                if (*tok) {
                    strncpy(folders[idx].files[folders[idx].file_count++], tok, MAX_FILENAME_LEN - 1);
                }
                tok = strtok_r(NULL, ",", &save);
            }
        }
    }
    fclose(f);
}

int folders_save_to_disk(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return -1;
    pthread_mutex_lock(&folders_mutex);
    for (int i = 0; i < folder_count; ++i) {
        fprintf(f, "%s|", folders[i].name);
        for (int j = 0; j < folders[i].file_count; ++j) {
            fprintf(f, "%s%s", folders[i].files[j], (j < folders[i].file_count - 1) ? "," : "");
        }
        fprintf(f, "\n");
    }
    pthread_mutex_unlock(&folders_mutex);
    fclose(f);
    return 0;
}

// ==================== REPLICATION (Fault Tolerance) ====================

typedef struct {
    char filename[MAX_FILENAME_LEN];
    int primary_ss_id;
    int replica_ids[MAX_STORAGE_SERVERS];
    int replica_count;
} ReplicaEntry;

static ReplicaEntry g_replicas[4096];
static int g_replica_count = 0;
static pthread_mutex_t g_replica_mutex = PTHREAD_MUTEX_INITIALIZER;

static int find_replica_idx(const char* filename) {
    for (int i=0; i<g_replica_count; ++i) {
        if (strcmp(g_replicas[i].filename, filename)==0) return i;
    }
    return -1;
}

int replicas_set(const char* filename, int primary_ss_id, const int *replica_ids, int replica_count) {
    if (!filename) return -1;
    pthread_mutex_lock(&g_replica_mutex);
    int idx = find_replica_idx(filename);
    if (idx < 0 && g_replica_count < (int)(sizeof(g_replicas)/sizeof(g_replicas[0]))) {
        idx = g_replica_count++;
        memset(&g_replicas[idx], 0, sizeof(ReplicaEntry));
        strncpy(g_replicas[idx].filename, filename, MAX_FILENAME_LEN-1);
    }
    if (idx >= 0) {
        g_replicas[idx].primary_ss_id = primary_ss_id;
        g_replicas[idx].replica_count = 0;
        for (int i=0; i<replica_count && i<MAX_STORAGE_SERVERS; ++i) {
            if (replica_ids[i] == primary_ss_id) continue;
            g_replicas[idx].replica_ids[g_replicas[idx].replica_count++] = replica_ids[i];
        }
        pthread_mutex_unlock(&g_replica_mutex);
        return 0;
    }
    pthread_mutex_unlock(&g_replica_mutex);
    return -1;
}

int replicas_get(const char* filename, int *out_ids, int max_ids) {
    if (!filename || !out_ids || max_ids<=0) return 0;
    pthread_mutex_lock(&g_replica_mutex);
    int idx = find_replica_idx(filename);
    int n=0;
    if (idx >= 0) {
        for (int i=0; i<g_replicas[idx].replica_count && n<max_ids; ++i) {
            out_ids[n++] = g_replicas[idx].replica_ids[i];
        }
    }
    pthread_mutex_unlock(&g_replica_mutex);
    return n;
}

int replicas_pick_alternate(int primary_ss_id) {
    // naive: pick first alive SS that is not primary
    pthread_mutex_lock(&ss_registry_mutex);
    for (int i=0; i<ss_count; ++i) {
        if (ss_registry[i].id == primary_ss_id) continue;
        if (ss_registry[i].is_alive) { int id = ss_registry[i].id; pthread_mutex_unlock(&ss_registry_mutex); return id; }
    }
    // if none alive, just pick any not equal
    for (int i=0; i<ss_count; ++i) { if (ss_registry[i].id != primary_ss_id) { int id=ss_registry[i].id; pthread_mutex_unlock(&ss_registry_mutex); return id; } }
    pthread_mutex_unlock(&ss_registry_mutex);
    return -1;
}

void set_storage_server_status(int ss_id, bool is_alive) {
    pthread_mutex_lock(&ss_registry_mutex);
    if (ss_id>=0 && ss_id<ss_count) { ss_registry[ss_id].is_alive = is_alive; if (is_alive) ss_registry[ss_id].last_heartbeat = time(NULL); }
    pthread_mutex_unlock(&ss_registry_mutex);
}

bool get_storage_server_status(int ss_id) {
    pthread_mutex_lock(&ss_registry_mutex);
    bool alive = false; if (ss_id>=0 && ss_id<ss_count) alive = ss_registry[ss_id].is_alive;
    pthread_mutex_unlock(&ss_registry_mutex);
    return alive;
}

int replicas_pick_alive_for_access(const char* filename, int primary_ss_id) {
    if (get_storage_server_status(primary_ss_id)) return primary_ss_id;
    int ids[MAX_STORAGE_SERVERS]; int n = replicas_get(filename, ids, MAX_STORAGE_SERVERS);
    for (int i=0; i<n; ++i) { if (get_storage_server_status(ids[i])) return ids[i]; }
    return -1;
}

