#include "proxy_parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#define MAX_BYTES 4096
#define MAX_CLIENTS 400
#define MAX_SIZE 200*(1<<20)
#define MAX_ELEMENT_SIZE 10*(1<<20)
#define MAX_BLOCKED_SITES 100
#define THROTTLE_RATE 1024 * 1024 // 1 MB/s

typedef struct cache_element cache_element;

struct cache_element {
    char* data;
    int len;
    char* url;
    time_t lru_time_track;
    cache_element* next;
};

char *blocked_sites[MAX_BLOCKED_SITES] = {
    "example.com",
    "blocked-site.com",
    NULL
};

int port_number = 8080;
int proxy_socketId;
pthread_t tid[MAX_CLIENTS];
sem_t seamaphore;
pthread_mutex_t lock;
cache_element* head;
int cache_size;

// Function prototypes
cache_element* find(char* url);
int add_cache_element(char* data, int size, char* url);
void remove_cache_element();
int is_url_blocked(const char *url);
void throttle_bandwidth(int socket, const char* data, int len);
void log_request(const char* method, const char* url, int status);
int handle_https_request(int client_socket, ParsedRequest *request);

// Existing functions (sendErrorMessage, connectRemoteServer, etc.) remain unchanged

int is_url_blocked(const char *url) {
    for (int i = 0; blocked_sites[i] != NULL; i++) {
        if (strstr(url, blocked_sites[i]) != NULL) {
            return 1;
        }
    }
    return 0;
}

void throttle_bandwidth(int socket, const char* data, int len) {
    int sent = 0;
    int to_send;
    time_t start, end;
    double elapsed;

    while (sent < len) {
        start = time(NULL);
        to_send = (len - sent < THROTTLE_RATE) ? (len - sent) : THROTTLE_RATE;
        send(socket, data + sent, to_send, 0);
        sent += to_send;
        end = time(NULL);
        elapsed = difftime(end, start);
        if (elapsed < 1.0) {
            usleep((1.0 - elapsed) * 1000000);
        }
    }
}

void log_request(const char* method, const char* url, int status) {
    time_t now = time(NULL);
    char* time_str = ctime(&now);
    time_str[strlen(time_str) - 1] = '\0'; // Remove newline
    printf("[%s] %s %s - Status: %d\n", time_str, method, url, status);
}

int handle_https_request(int client_socket, ParsedRequest *request) {
    int server_socket;
    char buffer[MAX_BYTES];
    int bytes_transferred;

    server_socket = connectRemoteServer(request->host, 443);
    if (server_socket < 0) {
        return -1;
    }

    // Send 200 Connection established to the client
    sprintf(buffer, "HTTP/1.1 200 Connection Established\r\n\r\n");
    send(client_socket, buffer, strlen(buffer), 0);

    // Use select to handle both sockets
    fd_set read_fds;
    int max_fd = (client_socket > server_socket) ? client_socket : server_socket;

    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(client_socket, &read_fds);
        FD_SET(server_socket, &read_fds);

        if (select(max_fd + 1, &read_fds, NULL, NULL, NULL) < 0) {
            perror("Select error");
            break;
        }

        if (FD_ISSET(client_socket, &read_fds)) {
            bytes_transferred = recv(client_socket, buffer, sizeof(buffer), 0);
            if (bytes_transferred <= 0) break;
            send(server_socket, buffer, bytes_transferred, 0);
        }

        if (FD_ISSET(server_socket, &read_fds)) {
            bytes_transferred = recv(server_socket, buffer, sizeof(buffer), 0);
            if (bytes_transferred <= 0) break;
            throttle_bandwidth(client_socket, buffer, bytes_transferred);
        }
    }

    close(server_socket);
    return 0;
}

void* thread_fn(void* socketNew) {
    sem_wait(&seamaphore);
    int p;
    sem_getvalue(&seamaphore, &p);
    printf("semaphore value:%d\n", p);
    int* t = (int*)(socketNew);
    int socket = *t;
    int bytes_send_client, len;

    char *buffer = (char*)calloc(MAX_BYTES, sizeof(char));
    
    bzero(buffer, MAX_BYTES);
    bytes_send_client = recv(socket, buffer, MAX_BYTES, 0);
    
    while(bytes_send_client > 0) {
        len = strlen(buffer);
        if(strstr(buffer, "\r\n\r\n") == NULL) {   
            bytes_send_client = recv(socket, buffer + len, MAX_BYTES - len, 0);
        } else {
            break;
        }
    }

    char *tempReq = (char*)malloc(strlen(buffer)*sizeof(char)+1);
    strcpy(tempReq, buffer);
    
    struct cache_element* temp = find(tempReq);

    if (temp != NULL) {
        throttle_bandwidth(socket, temp->data, temp->len);
        printf("Data retrieved from the Cache\n\n");
        log_request("GET", tempReq, 200);
    } else if(bytes_send_client > 0) {
        len = strlen(buffer); 
        ParsedRequest* request = ParsedRequest_create();
        
        if (ParsedRequest_parse(request, buffer, len) < 0) {
            printf("Parsing failed\n");
        } else {   
            bzero(buffer, MAX_BYTES);
            if(!strcmp(request->method, "GET") || !strcmp(request->method, "CONNECT")) {
                if (request->host && request->path && (checkHTTPversion(request->version) == 1)) {
                    if (is_url_blocked(request->host)) {
                        printf("Blocked request to: %s\n", request->host);
                        sendErrorMessage(socket, 403);
                        log_request(request->method, request->host, 403);
                    } else {
                        // Add custom header
                        ParsedHeader_set(request, "X-Proxy-Info", "Enhanced-Proxy-Server");

                        if (!strcmp(request->method, "CONNECT")) {
                            bytes_send_client = handle_https_request(socket, request);
                        } else {
                            bytes_send_client = handle_request(socket, request, tempReq);
                        }

                        if(bytes_send_client == -1) {   
                            sendErrorMessage(socket, 500);
                            log_request(request->method, request->host, 500);
                        } else {
                            log_request(request->method, request->host, 200);
                        }
                    }
                } else {
                    sendErrorMessage(socket, 500);
                    log_request(request->method, request->host ? request->host : "Unknown", 500);
                }
            } else {
                printf("This code doesn't support any method other than GET and CONNECT\n");
                log_request(request->method, request->host ? request->host : "Unknown", 501);
            }
        }
        ParsedRequest_destroy(request);
    }

    shutdown(socket, SHUT_RDWR);
    close(socket);
    free(buffer);
    sem_post(&seamaphore);  
    
    sem_getvalue(&seamaphore, &p);
    printf("Semaphore post value:%d\n", p);
    free(tempReq);
    return NULL;
}

int main(int argc, char * argv[]) {
    int client_socketId, client_len;
    struct sockaddr_in server_addr, client_addr;

    sem_init(&seamaphore, 0, MAX_CLIENTS);
    pthread_mutex_init(&lock, NULL);

    if(argc == 2) {
        port_number = atoi(argv[1]);
    } else {
        printf("Too few arguments\n");
        exit(1);
    }

    printf("Setting Proxy Server Port : %d\n", port_number);

    proxy_socketId = socket(AF_INET, SOCK_STREAM, 0);

    if(proxy_socketId < 0) {
        perror("Failed to create socket.\n");
        exit(1);
    }

    int reuse = 1;
    if (setsockopt(proxy_socketId, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse)) < 0) 
        perror("setsockopt(SO_REUSEADDR) failed\n");

    bzero((char*)&server_addr, sizeof(server_addr));  
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_number);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if(bind(proxy_socketId, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Port is not free\n");
        exit(1);
    }
    printf("Binding on port: %d\n", port_number);

    int listen_status = listen(proxy_socketId, MAX_CLIENTS);

    if(listen_status < 0) {
        perror("Error while Listening !\n");
        exit(1);
    }

    int i = 0;
    int Connected_socketId[MAX_CLIENTS];

    while(1) {
        bzero((char*)&client_addr, sizeof(client_addr));
        client_len = sizeof(client_addr); 

        client_socketId = accept(proxy_socketId, (struct sockaddr*)&client_addr, (socklen_t*)&client_len);
        if(client_socketId < 0) {
            fprintf(stderr, "Error in Accepting connection !\n");
            exit(1);
        } else {
            Connected_socketId[i] = client_socketId;
        }

        struct sockaddr_in* client_pt = (struct sockaddr_in*)&client_addr;
        struct in_addr ip_addr = client_pt->sin_addr;
        char str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &ip_addr, str, INET_ADDRSTRLEN);
        printf("Client is connected with port number: %d and ip address: %s \n", ntohs(client_addr.sin_port), str);

        pthread_create(&tid[i], NULL, thread_fn, (void*)&Connected_socketId[i]);
        i++; 
    }
    close(proxy_socketId);
    return 0;
}

