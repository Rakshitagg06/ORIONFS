#include "network.h"
#include "utils.h"
#include <fcntl.h>
#include <errno.h>
#include <sys/time.h>

// ==================== SOCKET OPERATIONS ====================

int create_tcp_socket() {
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        perror("Socket creation failed");
        return -1;
    }
    
    // Set socket options to reuse address
    int opt = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("Setsockopt failed");
        close(socket_fd);
        return -1;
    }
    
    return socket_fd;
}

int bind_socket(int socket_fd, int port) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(socket_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("Bind failed");
        return -1;
    }
    
    return 0;
}

int listen_socket(int socket_fd, int backlog) {
    if (listen(socket_fd, backlog) < 0) {
        perror("Listen failed");
        return -1;
    }
    return 0;
}

int accept_connection(int socket_fd, char* client_ip, int* client_port) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    
    int client_socket = accept(socket_fd, (struct sockaddr*)&client_addr, &addr_len);
    if (client_socket < 0) {
        perror("Accept failed");
        return -1;
    }
    
    if (client_ip != NULL) {
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, MAX_IP_LEN);
    }
    
    if (client_port != NULL) {
        *client_port = ntohs(client_addr.sin_port);
    }
    
    return client_socket;
}

int connect_to_server(const char* ip, int port) {
    int socket_fd = create_tcp_socket();
    if (socket_fd < 0) return -1;
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        perror("Invalid address");
        close(socket_fd);
        return -1;
    }
    
    if (connect(socket_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection failed");
        close(socket_fd);
        return -1;
    }
    
    return socket_fd;
}

void close_socket(int socket_fd) {
    if (socket_fd >= 0) {
        close(socket_fd);
    }
}

// ==================== MESSAGE SEND/RECEIVE ====================

int send_message(int socket_fd, Message* msg) {
    if (msg == NULL) return -1;
    
    const char* buf = (const char*)msg;
    size_t total = 0;
    size_t len = sizeof(Message);
    while (total < len) {
        ssize_t sent = send(socket_fd, buf + total, len - total, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) continue;
            perror("Send failed");
            return -1;
        }
        if (sent == 0) {
            // Connection closed
            return -1;
        }
        total += (size_t)sent;
    }
    return (int)total;
}

int recv_message(int socket_fd, Message* msg) {
    if (msg == NULL) return -1;
    
    char* buf = (char*)msg;
    size_t total = 0;
    size_t len = sizeof(Message);
    while (total < len) {
        ssize_t recvd = recv(socket_fd, buf + total, len - total, 0);
        if (recvd < 0) {
            if (errno == EINTR) continue;
            // Suppress noisy EAGAIN/EWOULDBLOCK (timeout/non-blocking) as they are expected when using SO_RCVTIMEO.
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return -1; // indicate timeout/no data without printing error
            }
            perror("Receive failed");
            return -1;
        }
        if (recvd == 0) {
            // Connection closed
            if (total == 0) return 0; // indicate closed
            return -1; // partial then closed, treat as error
        }
        total += (size_t)recvd;
    }
    return (int)total;
}

int send_data(int socket_fd, const char* data, int length) {
    if (data == NULL || length <= 0) return -1;
    
    ssize_t sent = send(socket_fd, data, length, MSG_NOSIGNAL);
    if (sent < 0) {
        perror("Send data failed");
        return -1;
    }
    
    return sent;
}

int recv_data(int socket_fd, char* buffer, int max_length) {
    if (buffer == NULL || max_length <= 0) return -1;
    
    memset(buffer, 0, max_length);
    
    ssize_t received = recv(socket_fd, buffer, max_length - 1, 0);
    if (received < 0) {
        perror("Receive data failed");
        return -1;
    }
    
    return received;
}

int send_string(int socket_fd, const char* str) {
    if (str == NULL) return -1;
    
    int length = strlen(str);
    
    // Send length first
    if (send(socket_fd, &length, sizeof(int), MSG_NOSIGNAL) < 0) {
        return -1;
    }
    
    // Then send string
    if (send(socket_fd, str, length, MSG_NOSIGNAL) < 0) {
        return -1;
    }
    
    return length;
}

int recv_string(int socket_fd, char* buffer, int max_length) {
    if (buffer == NULL || max_length <= 0) return -1;
    
    int length;
    
    // Receive length first
    if (recv(socket_fd, &length, sizeof(int), 0) < 0) {
        return -1;
    }
    
    if (length >= max_length) {
        length = max_length - 1;
    }
    
    // Receive string
    if (recv(socket_fd, buffer, length, 0) < 0) {
        return -1;
    }
    
    buffer[length] = '\0';
    return length;
}

// ==================== UTILITY FUNCTIONS ====================

int set_non_blocking(int socket_fd) {
    int flags = fcntl(socket_fd, F_GETFL, 0);
    if (flags < 0) return -1;
    
    if (fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }
    
    return 0;
}

int set_socket_timeout(int socket_fd, int seconds) {
    struct timeval timeout;
    timeout.tv_sec = seconds;
    timeout.tv_usec = 0;
    
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        return -1;
    }
    
    if (setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        return -1;
    }
    
    return 0;
}

void get_local_ip(char* ip_buffer, int buffer_size) {
    // Simple implementation - get IP by connecting to external server
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        strncpy(ip_buffer, "127.0.0.1", buffer_size);
        return;
    }
    
    struct sockaddr_in serv;
    memset(&serv, 0, sizeof(serv));
    serv.sin_family = AF_INET;
    serv.sin_addr.s_addr = inet_addr("8.8.8.8");
    serv.sin_port = htons(53);
    
    connect(sock, (struct sockaddr*)&serv, sizeof(serv));
    
    struct sockaddr_in name;
    socklen_t namelen = sizeof(name);
    getsockname(sock, (struct sockaddr*)&name, &namelen);
    
    inet_ntop(AF_INET, &name.sin_addr, ip_buffer, buffer_size);
    close(sock);
}

bool is_socket_alive(int socket_fd) {
    char buffer[1];
    ssize_t result = recv(socket_fd, buffer, 1, MSG_PEEK | MSG_DONTWAIT);
    
    if (result == 0) {
        return false;  // Connection closed
    }
    
    if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        return false;  // Error occurred
    }
    
    return true;
}

// ==================== SERVER UTILITIES ====================

int start_server(int port, int backlog) {
    int socket_fd = create_tcp_socket();
    if (socket_fd < 0) return -1;
    
    if (bind_socket(socket_fd, port) < 0) {
        close_socket(socket_fd);
        return -1;
    }
    
    if (listen_socket(socket_fd, backlog) < 0) {
        close_socket(socket_fd);
        return -1;
    }
    
    printf("Server started on port %d\n", port);
    return socket_fd;
}

void stop_server(int socket_fd) {
    close_socket(socket_fd);
    printf("Server stopped\n");
}
