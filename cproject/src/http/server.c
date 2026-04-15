#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include "server.h"

void start_server(int port) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    
    bind(server_fd, (struct sockaddr*)&address, sizeof(address));
    listen(server_fd, 10);
    
    printf("ポート %d で待機中\n", port);
    
    while(1) {
        int new_socket = accept(server_fd, NULL, NULL);
        char buffer[1024] = {0};
        read(new_socket, buffer, 1024);
        
        const char* response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nHello from C!";
        write(new_socket, response, strlen(response));
        close(new_socket);
    }
}
