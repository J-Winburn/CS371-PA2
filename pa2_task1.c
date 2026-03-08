/*
# Copyright 2025 University of Kentucky
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0
*/

/* 
Please specify the group members here

# Student #1: Jared Winburn

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pthread.h>

#define MAX_EVENTS 64
#define MESSAGE_SIZE 16
#define DEFAULT_CLIENT_THREADS 4
#define PIPELINE_SIZE 10

char *server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
int num_requests = 1000000;

typedef struct {
    int epoll_fd;
    int socket_fd;
    long long total_rtt;
    long total_messages;
    float request_rate;
    long tx_cnt;
    long rx_cnt;
    long lost_pkt_cnt; // calc by doing tx_cnt - rx_cnt to get total lost packet count 
} client_thread_data_t;

typedef struct {
    uint32_t seq_num;
    char data[12];
}packet_t;


void *client_thread_func(void *arg) {
    client_thread_data_t *data = (client_thread_data_t *)arg;
    struct epoll_event event, events[MAX_EVENTS];
    char send_buf[MESSAGE_SIZE] = "ABCDEFGHIJKMLNOP";
    char recv_buf[MESSAGE_SIZE];
    struct timeval start, end;

    data->request_rate = 0;
    data->total_rtt = 0;
    data->total_messages = 0;
    data->tx_cnt = 0;
    data->rx_cnt = 0;
    data->lost_pkt_cnt = 0;

    // Register the socket in the epoll instance
    event.events = EPOLLIN;
    event.data.fd = data->socket_fd;
    if(epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->socket_fd, &event) < 0){
        perror("epoll_ctl");
        return NULL;
    }

    while (data->tx_cnt < num_requests) {
        // Send a message
        int to_send = PIPELINE_SIZE;

        if (data->tx_cnt + to_send > num_requests){
            to_send = num_requests - data->tx_cnt;
        }

        gettimeofday(&start,NULL);

        for(int i = 0; i < to_send; i++){
            if(send(data->socket_fd, send_buf, MESSAGE_SIZE, 0) < 0){
                perror("Send Failed");
                break;
            }
            data->tx_cnt++;
        }

        int expected_replies = to_send;
        while(expected_replies > 0){
            int n_events = epoll_wait(data->epoll_fd, events, MAX_EVENTS, 500); // wait 500ms for replies

            if (n_events < 0) { 
                perror("Epoll Wait Failed");
                break;
            }
            if(n_events == 0){ // timeout, no replies recieved in 500ms
                break;
            }
            for (int i = 0; i < n_events; i++){
                if(events[i].data.fd == data->socket_fd){
                    int n = recv(data->socket_fd, recv_buf, MESSAGE_SIZE, 0);
                    if (n > 0){
                        data->rx_cnt++;
                        data->total_messages++;
                        expected_replies--;
                    }
                }
            }
        }
        gettimeofday(&end, NULL); //accumulate RTT for entire pipeline 

        long long rtt = (end.tv_sec - start.tv_sec) * 1000000LL + (end.tv_usec - start.tv_usec);
        data->total_rtt += rtt;

    }
    data->lost_pkt_cnt = data->tx_cnt - data->rx_cnt;
    
    if (data->total_rtt > 0){ // if all packets are lost, avoide division by 0 
        data->request_rate = (float)data->total_messages * 1000000.0f / (float)data->total_rtt;
    }
    close(data->socket_fd);
    close(data->epoll_fd);
    return NULL;
}

void run_client() {
    pthread_t threads[num_client_threads];
    client_thread_data_t thread_data[num_client_threads];
    struct sockaddr_in server_addr;

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr);

    // Create client threads
    for (int i = 0; i < num_client_threads; i++) {
        thread_data[i].epoll_fd = epoll_create1(0);
        if (thread_data[i].epoll_fd < 0){
            perror("Epoll Create Failed");
            exit(EXIT_FAILURE);
        }
        thread_data[i].socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (thread_data[i].socket_fd < 0){
            perror("Socket Failed");
            exit(EXIT_FAILURE);
        }
        if (connect(thread_data[i].socket_fd, (struct sockaddr*) &server_addr, sizeof(server_addr)) < 0 ){
            perror("Connect Failure");
            exit(EXIT_FAILURE);
        }
    }

    for (int i = 0; i < num_client_threads; i++) {
        pthread_create(&threads[i], NULL, client_thread_func, &thread_data[i]);
    }

    // Wait for threads to complete
    long long total_rtt = 0;
    long total_messages = 0;
    float total_request_rate = 0;
    long total_tx = 0;
    long total_rx = 0;
    long total_lost = 0;

    for (int i = 0; i < num_client_threads; i++) {
        pthread_join(threads[i], NULL);
        total_rtt += thread_data[i].total_rtt;
        total_messages += thread_data[i].total_messages;
        total_request_rate += thread_data[i].request_rate;
        total_tx += thread_data[i].tx_cnt;
        total_rx += thread_data[i].rx_cnt;
        total_lost += thread_data[i].lost_pkt_cnt;
    }

    printf("Average RTT: %lld us\n", total_messages > 0 ? total_rtt / total_messages : 0);
    printf("Total Request Rate: %f messages/s\n", total_request_rate);
    printf("Total Sent: %ld\n", total_tx);
    printf("Total Received: %ld\n", total_rx);
    printf("Total Lost: %ld\n", total_lost);
    printf("Packet Loss Rate: %f%%\n", total_tx > 0 ? (float)total_lost / (float)total_tx * 100.0f : 0.0f);
}

void run_server() {
    int server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if(server_fd < 0){
        perror("Socket Failed");
        exit(EXIT_FAILURE);
    }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));


    struct sockaddr_in server_addr;

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(server_port);

    if(bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0){
        perror("Bind Failed");
        exit(EXIT_FAILURE);
    }
    int epoll_fd = epoll_create1(0);
    if(epoll_fd < 0){
        perror("Epoll Create Failed");
        exit(EXIT_FAILURE);
    }

    struct epoll_event event, events[MAX_EVENTS];

    event.events = EPOLLIN;
    event.data.fd = server_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event);

    printf("Server listening on port %d...\n ", server_port);

    while (1) {
        int n_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (n_events <  0){
            perror("Epoll Wait Failed");
            break;
        }
        for (int i = 0; i < n_events; i++) {
            if (events[i].data.fd == server_fd) {
                char buffer[MESSAGE_SIZE];
                struct sockaddr_in client_addr;
                socklen_t addr_len = sizeof(client_addr);

                int n = recvfrom(server_fd, buffer, MESSAGE_SIZE, 0, (struct sockaddr*) &client_addr, &addr_len);
                if (n < 0){
                    perror("Recieve From Failed");
                    continue;
                }
                if (sendto(server_fd, buffer, n,0,(struct sockaddr*) &client_addr, addr_len) < 0){
                    perror("Send To Failed");

                }
            }
        }
    }

    close(server_fd);
    close(epoll_fd);
}

int main(int argc, char *argv[]) {
    if (argc > 1 && strcmp(argv[1], "server") == 0) {
        if (argc > 2) server_ip = argv[2];
        if (argc > 3) server_port = atoi(argv[3]);

        run_server();
    } else if (argc > 1 && strcmp(argv[1], "client") == 0) {
        if (argc > 2) server_ip = argv[2];
        if (argc > 3) server_port = atoi(argv[3]);
        if (argc > 4) num_client_threads = atoi(argv[4]);
        if (argc > 5) num_requests = atoi(argv[5]);

        run_client();
    } else {
        printf("Usage: %s <server|client> [server_ip server_port num_client_threads num_requests]\n", argv[0]);
    }

    return 0;
}