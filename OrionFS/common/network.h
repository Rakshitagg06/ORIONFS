#ifndef NETWORK_H
#define NETWORK_H

#include "protocol.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

// ==================== SOCKET OPERATIONS ====================

// Create a TCP socket
int create_tcp_socket();

// Bind socket to address and port
int bind_socket(int socket_fd, int port);

// Listen for incoming connections
int listen_socket(int socket_fd, int backlog);

// Accept incoming connection
int accept_connection(int socket_fd, char* client_ip, int* client_port);

// Connect to server
int connect_to_server(const char* ip, int port);

// Close socket
void close_socket(int socket_fd);

// ==================== MESSAGE SEND/RECEIVE ====================

// Send message over socket
int send_message(int socket_fd, Message* msg);

// Receive message from socket
int recv_message(int socket_fd, Message* msg);

// Send raw data
int send_data(int socket_fd, const char* data, int length);

// Receive raw data
int recv_data(int socket_fd, char* buffer, int max_length);

// Send string (with length prefix)
int send_string(int socket_fd, const char* str);

// Receive string (with length prefix)
int recv_string(int socket_fd, char* buffer, int max_length);

// ==================== UTILITY FUNCTIONS ====================

// Set socket to non-blocking mode
int set_non_blocking(int socket_fd);

// Set socket timeout
int set_socket_timeout(int socket_fd, int seconds);

// Get local IP address
void get_local_ip(char* ip_buffer, int buffer_size);

// Check if socket is alive
bool is_socket_alive(int socket_fd);

// ==================== SERVER UTILITIES ====================

// Start server (create, bind, listen)
int start_server(int port, int backlog);

// Stop server
void stop_server(int socket_fd);

#endif // NETWORK_H
