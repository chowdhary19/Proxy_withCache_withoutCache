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
#include <openssl/ssl.h>
#include <openssl/err.h>

#define MAX_BYTES 4096
#define MAX_CLIENTS 400
#define MAX_LOG_SIZE 1024
#define THROTTLE_RATE 1024 * 1024 // 1 MB/s

const char* blocked_sites[] = {
    "example.com",
    "blockedsite.com",
    // Add more blocked sites here
    NULL
};

int port_number = 8080;
int proxy_socketId;
pthread_t tid[MAX_CLIENTS];
sem_t semaphore;

int sendErrorMessage(int socket, int status_code)
{
    char str[1024];
    char currentTime[50];
    time_t now = time(0);

    struct tm data = *gmtime(&now);
    strftime(currentTime, sizeof(currentTime), "%a, %d %b %Y %H:%M:%S %Z", &data);

    switch(status_code)
    {
        case 400: snprintf(str, sizeof(str), "HTTP/1.1 400 Bad Request\r\nContent-Length: 95\r\nConnection: close\r\nContent-Type: text/html\r\nDate: %s\r\nServer: MyProxy/1.0\r\n\r\n<HTML><HEAD><TITLE>400 Bad Request</TITLE></HEAD>\n<BODY><H1>400 Bad Request</H1>\n</BODY></HTML>", currentTime);
                  printf("400 Bad Request\n");
                  send(socket, str, strlen(str), 0);
                  break;

        case 403: snprintf(str, sizeof(str), "HTTP/1.1 403 Forbidden\r\nContent-Length: 112\r\nContent-Type: text/html\r\nConnection: close\r\nDate: %s\r\nServer: MyProxy/1.0\r\n\r\n<HTML><HEAD><TITLE>403 Forbidden</TITLE></HEAD>\n<BODY><H1>403 Forbidden</H1><br>Permission Denied\n</BODY></HTML>", currentTime);
                  printf("403 Forbidden\n");
                  send(socket, str, strlen(str), 0);
                  break;

        case 404: snprintf(str, sizeof(str), "HTTP/1.1 404 Not Found\r\nContent-Length: 91\r\nContent-Type: text/html\r\nConnection: close\r\nDate: %s\r\nServer: MyProxy/1.0\r\n\r\n<HTML><HEAD><TITLE>404 Not Found</TITLE></HEAD>\n<BODY><H1>404 Not Found</H1>\n</BODY></HTML>", currentTime);
                  printf("404 Not Found\n");
                  send(socket, str, strlen(str), 0);
                  break;

        case 500: snprintf(str, sizeof(str), "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 115\r\nConnection: close\r\nContent-Type: text/html\r\nDate: %s\r\nServer: MyProxy/1.0\r\n\r\n<HTML><HEAD><TITLE>500 Internal Server Error</TITLE></HEAD>\n<BODY><H1>500 Internal Server Error</H1>\n</BODY></HTML>", currentTime);
                  send(socket, str, strlen(str), 0);
                  break;

        case 501: snprintf(str, sizeof(str), "HTTP/1.1 501 Not Implemented\r\nContent-Length: 103\r\nConnection: close\r\nContent-Type: text/html\r\nDate: %s\r\nServer: MyProxy/1.0\r\n\r\n<HTML><HEAD><TITLE>501 Not Implemented</TITLE></HEAD>\n<BODY><H1>501 Not Implemented</H1>\n</BODY></HTML>", currentTime);
                  printf("501 Not Implemented\n");
                  send(socket, str, strlen(str), 0);
                  break;

        case 505: snprintf(str, sizeof(str), "HTTP/1.1 505 HTTP Version Not Supported\r\nContent-Length: 125\r\nConnection: close\r\nContent-Type: text/html\r\nDate: %s\r\nServer: MyProxy/1.0\r\n\r\n<HTML><HEAD><TITLE>505 HTTP Version Not Supported</TITLE></HEAD>\n<BODY><H1>505 HTTP Version Not Supported</H1>\n</BODY></HTML>", currentTime);
                  printf("505 HTTP Version Not Supported\n");
                  send(socket, str, strlen(str), 0);
                  break;

        default:  return -1;
    }
    return 1;
}

int connectRemoteServer(char* host_addr, int port_num)
{
    int remoteSocket = socket(AF_INET, SOCK_STREAM, 0);

    if(remoteSocket < 0)
    {
        printf("Error in Creating Socket.\n");
        return -1;
    }
    
    struct hostent *host = gethostbyname(host_addr);    
    if(host == NULL)
    {
        fprintf(stderr, "No such host exists.\n");    
        return -1;
    }

    struct sockaddr_in server_addr;

    bzero((char*)&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_num);

    bcopy((char *)host->h_addr, (char *)&server_addr.sin_addr.s_addr, host->h_length);

    if(connect(remoteSocket, (struct sockaddr*)&server_addr, (socklen_t)sizeof(server_addr)) < 0)
    {
        fprintf(stderr, "Error in connecting!\n"); 
        return -1;
    }

    return remoteSocket;
}

SSL_CTX* create_ssl_context() {
    SSL_CTX *ctx;

    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();
    ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        ERR_print_errors_fp(stderr);
        exit(2);
    }
    return ctx;
}

int is_url_blocked(const char* url) {
    for (int i = 0; blocked_sites[i] != NULL; i++) {
        if (strstr(url, blocked_sites[i]) != NULL) {
            return 1;
        }
    }
    return 0;
}

void log_request(const char* client_ip, const char* url, int status) {
    FILE* log_file = fopen("proxy.log", "a");
    if (log_file) {
        time_t now = time(NULL);
        char time_str[64];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
        fprintf(log_file, "[%s] %s - %s - Status: %d\n", time_str, client_ip, url, status);
        fclose(log_file);
    }
}

void throttle_bandwidth(int socket, int bytes_to_send) {
    static time_t last_check = 0;
    static int bytes_sent = 0;
    
    time_t now = time(NULL);
    if (now != last_check) {
        bytes_sent = 0;
        last_check = now;
    }
    
    bytes_sent += bytes_to_send;
    if (bytes_sent > THROTTLE_RATE) {
        usleep(1000000); // Sleep for 1 second
        bytes_sent = 0;
    }
}

void modify_request_headers(struct ParsedRequest *request) {
    ParsedHeader_set(request, "X-Forwarded-By", "MyProxy/1.0");
}

int handle_https_request(int client_socket, struct ParsedRequest *request) {
    char buffer[MAX_BYTES];
    int server_socket = connectRemoteServer(request->host, 443);
    if (server_socket < 0) return -1;

    snprintf(buffer, sizeof(buffer), "HTTP/1.1 200 Connection Established\r\n\r\n");
    send(client_socket, buffer, strlen(buffer), 0);

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
            int bytes = recv(client_socket, buffer, sizeof(buffer), 0);
            if (bytes <= 0) break;
            send(server_socket, buffer, bytes, 0);
        }

        if (FD_ISSET(server_socket, &read_fds)) {
            int bytes = recv(server_socket, buffer, sizeof(buffer), 0);
            if (bytes <= 0) break;
            throttle_bandwidth(client_socket, bytes);
            send(client_socket, buffer, bytes, 0);
        }
    }

    close(server_socket);
    return 0;
}

int handle_request(int clientSocket, struct ParsedRequest *request, char *buf)
{
    strcpy(buf, request->method);
    strcat(buf, " ");
    strcat(buf, request->path);
    strcat(buf, " ");
    strcat(buf, request->version);
    strcat(buf, "\r\n");

    size_t len = strlen(buf);

    if (ParsedHeader_set(request, "Connection", "close") < 0) {
        printf("set header key not work\n");
    }

    if(ParsedHeader_get(request, "Host") == NULL) {
        if(ParsedHeader_set(request, "Host", request->host) < 0) {
            printf("Set \"Host\" header key not working\n");
        }
    }

    modify_request_headers(request);

    if (ParsedRequest_unparse_headers(request, buf + len, (size_t)MAX_BYTES - len) < 0) {
        printf("unparse failed\n");
    }

    int server_port = request->port ? atoi(request->port) : 80;

    if (is_url_blocked(request->host)) {
        sendErrorMessage(clientSocket, 403);
        log_request("client_ip", request->host, 403);
        return -1;
    }

    int remoteSocketID = connectRemoteServer(request->host, server_port);
    if(remoteSocketID < 0) return -1;

    SSL_CTX *ctx = NULL;
    SSL *ssl = NULL;
    if (server_port == 443) {
        ctx = create_ssl_context();
        ssl = SSL_new(ctx);
        SSL_set_fd(ssl, remoteSocketID);
        if (SSL_connect(ssl) <= 0) {
            ERR_print_errors_fp(stderr);
            return -1;
        }
    }

    int bytes_send = ssl ? SSL_write(ssl, buf, strlen(buf)) : send(remoteSocketID, buf, strlen(buf), 0);

    bzero(buf, MAX_BYTES);

    while(1) {
        bytes_send = ssl ? SSL_read(ssl, buf, MAX_BYTES-1) : recv(remoteSocketID, buf, MAX_BYTES-1, 0);
        if (bytes_send <= 0) break;
        throttle_bandwidth(clientSocket, bytes_send);
        send(clientSocket, buf, bytes_send, 0);
        bzero(buf, MAX_BYTES);
    }

    if (ssl) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
    }

    close(remoteSocketID);
    log_request("client_ip", request->host, 200);
    return 0;
}

int checkHTTPversion(char *msg)
{
    return (strncmp(msg, "HTTP/1.1", 8) == 0 || strncmp(msg, "HTTP/1.0", 8) == 0) ? 1 : -1;
}

void* thread_fn(void* socketNew)
{
    sem_wait(&semaphore);
    int socket = *(int*)socketNew;
    int bytes_recv;
    char *buffer = (char*)calloc(MAX_BYTES, sizeof(char));
    
    bytes_recv = recv(socket, buffer, MAX_BYTES, 0);
    while(bytes_recv > 0 && strstr(buffer, "\r\n\r\n") == NULL) {
        bytes_recv += recv(socket, buffer + bytes_recv, MAX_BYTES - bytes_recv, 0);
    }
    
    if(bytes_recv > 0) {
        struct ParsedRequest* request = ParsedRequest_create();
        if (ParsedRequest_parse(request, buffer, strlen(buffer)) < 0) {
            printf("Parsing failed\n");
        } else {
            bzero(buffer, MAX_BYTES);
            if(!strcmp(request->method, "GET") || !strcmp(request->method, "CONNECT")) {
                if (request->host && request->path && (checkHTTPversion(request->version) == 1)) {
                    if (!strcmp(request->method, "CONNECT")) {
                        handle_https_request(socket, request);
                    } else {
                        handle_request(socket, request, buffer);
                    }
                } else {
                    sendErrorMessage(socket, 400);
                }
            } else {
                sendErrorMessage(socket, 501);
            }
        }
        ParsedRequest_destroy(request);
    } else if(bytes_recv < 0) {
        perror("Error in receiving from client.\n");
    }

    shutdown(socket, SHUT_RDWR);
    close(socket);
    free(buffer);
    sem_post(&semaphore);
    return NULL;
}

int main(int argc, char * argv[]) {
    int client_socketId, client_len;
    struct sockaddr_in server_addr, client_addr;

    sem_init(&semaphore, 0, MAX_CLIENTS);

    if(argc == 2) {
        port_number = atoi(argv[1]);
    } else {
        printf("Usage: %s <port_number>\n", argv[0]);
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

    if(listen(proxy_socketId, MAX_CLIENTS) < 0) {
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
            continue;
        }

        Connected_socketId[i] = client_socketId;

        struct sockaddr_in* client_pt = (struct sockaddr_in*)&client_addr;
        struct in_addr ip_addr = client_pt->sin_addr;
        char str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &ip_addr, str, INET_ADDRSTRLEN);
        printf("Client connected with port number: %d and ip address: %s\n", ntohs(client_addr.sin_port), str);

        pthread_create(&tid[i], NULL, thread_fn, (void*)&Connected_socketId[i]);
        i = (i + 1) % MAX_CLIENTS;
    }

    close(proxy_socketId);
    return 0;
}