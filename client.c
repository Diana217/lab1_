#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <poll.h>

#define BUF_LEN 1024
#define UNIX_SOCK_PATH "/tmp/perf_test_socket"

typedef enum {
    SOCK_UNIX, SOCK_INET
} sock_type;

typedef enum {
    SYNC_BLOCKING, SYNC_NONBLOCKING, ASYNC_BLOCKING, ASYNC_NONBLOCKING
} sock_mode;

typedef struct {
    sock_type type;
    sock_mode mode;
    char *addr;
    int port;
    int workload_size;
} client_config;

int main(int argc, char *argv[]) {
    client_config config;
    config.type = SOCK_UNIX;
    config.mode = SYNC_BLOCKING;
    config.addr = "127.0.0.1";
    config.port = 8080;
    config.workload_size = 100000;

    static struct option long_opts[] = {
            {"type",     required_argument, 0, 0},
            {"mode",     required_argument, 0, 0},
            {"workload", required_argument, 0, 0},
            {0, 0,                          0, 0}
    };

    int option_index = 0;
    while (1) {
        int c = getopt_long(argc, argv, "", long_opts, &option_index);
        if (c == -1) {
            break;
        }

        if (c == 0) {
            if (strcmp(long_opts[option_index].name, "type") == 0) {
                if (strcmp(optarg, "unix") == 0)
                    config.type = SOCK_UNIX;
                else if (strcmp(optarg, "inet") == 0)
                    config.type = SOCK_INET;
                else {
                    fprintf(stderr, "Unsupported socket type: %s\n", optarg);
                    exit(EXIT_FAILURE);
                }
            } else if (strcmp(long_opts[option_index].name, "mode") == 0) {
                if (strcmp(optarg, "blocking-sync") == 0)
                    config.mode = SYNC_BLOCKING;
                else if (strcmp(optarg, "nonblocking-sync") == 0)
                    config.mode = SYNC_NONBLOCKING;
                else if (strcmp(optarg, "blocking-async") == 0)
                    config.mode = ASYNC_BLOCKING;
                else if (strcmp(optarg, "nonblocking-async") == 0)
                    config.mode = ASYNC_NONBLOCKING;
                else {
                    fprintf(stderr, "Invalid mode specified: %s\n", optarg);
                    exit(EXIT_FAILURE);
                }
            } else if (strcmp(long_opts[option_index].name, "workload") == 0) {
                config.workload_size = atoi(optarg);
                if (config.workload_size <= 0) {
                    fprintf(stderr, "Invalid workload size: %s\n", optarg);
                    exit(EXIT_FAILURE);
                }
            }
        } else {
            fprintf(stderr, "Unknown option\n");
            exit(EXIT_FAILURE);
        }
    }

    int sockfd;
    struct sockaddr_un unix_addr;
    struct sockaddr_in inet_addr;

    struct timespec t_conn_start, t_conn_end;
    clock_gettime(CLOCK_MONOTONIC, &t_conn_start);

    if (config.type == SOCK_UNIX) {
        sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sockfd == -1) {
            perror("Creating UNIX socket failed");
            exit(EXIT_FAILURE);
        }
        memset(&unix_addr, 0, sizeof(unix_addr));
        unix_addr.sun_family = AF_UNIX;
        strncpy(unix_addr.sun_path, UNIX_SOCK_PATH, sizeof(unix_addr.sun_path) - 1);

        if (connect(sockfd, (struct sockaddr *) &unix_addr, sizeof(unix_addr)) == -1) {
            perror("Connecting to UNIX socket failed");
            close(sockfd);
            exit(EXIT_FAILURE);
        }
    } else {
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd == -1) {
            perror("Creating INET socket failed");
            exit(EXIT_FAILURE);
        }
        memset(&inet_addr, 0, sizeof(inet_addr));
        inet_addr.sin_family = AF_INET;
        inet_addr.sin_port = htons(config.port);
        if (inet_pton(AF_INET, config.addr, &inet_addr.sin_addr) <= 0) {
            perror("Invalid IP address");
            close(sockfd);
            exit(EXIT_FAILURE);
        }

        if (connect(sockfd, (struct sockaddr *) &inet_addr, sizeof(inet_addr)) == -1) {
            perror("Connecting to INET socket failed");
            close(sockfd);
            exit(EXIT_FAILURE);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t_conn_end);
    double conn_duration = (t_conn_end.tv_sec - t_conn_start.tv_sec) * 1000.0 +
                           (t_conn_end.tv_nsec - t_conn_start.tv_nsec) / 1e6;

    if (config.mode == SYNC_NONBLOCKING || config.mode == ASYNC_NONBLOCKING) {
        int flags = fcntl(sockfd, F_GETFL, 0);
        if (flags == -1) {
            perror("Getting socket flags failed");
            close(sockfd);
            exit(EXIT_FAILURE);
        }
        if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) {
            perror("Setting non-blocking mode failed");
            close(sockfd);
            exit(EXIT_FAILURE);
        }
    }

    char msg_buffer[BUF_LEN];
    memset(msg_buffer, 'A', BUF_LEN);

    struct timespec t_data_start, t_data_end;
    clock_gettime(CLOCK_MONOTONIC, &t_data_start);

    int packets_sent = 0, bytes_sent = 0;

    if (config.mode == ASYNC_BLOCKING || config.mode == ASYNC_NONBLOCKING) {
        int epfd = epoll_create1(0);
        if (epfd == -1) {
            perror("Epoll creation failed");
            close(sockfd);
            exit(EXIT_FAILURE);
        }

        struct epoll_event event = {.events = EPOLLOUT, .data.fd = sockfd};
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, sockfd, &event) == -1) {
            perror("Adding socket to epoll failed");
            close(epfd);
            close(sockfd);
            exit(EXIT_FAILURE);
        }

        while (packets_sent < config.workload_size) {
            struct epoll_event events[1];
            int n_events = epoll_wait(epfd, events, 1, -1);
            if (n_events == -1) {
                perror("Epoll wait failed");
                close(epfd);
                close(sockfd);
                exit(EXIT_FAILURE);
            }

            for (int i = 0; i < n_events; i++) {
                if (events[i].events & EPOLLOUT) {
                    ssize_t sent = send(sockfd, msg_buffer, BUF_LEN, 0);
                    if (sent == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            continue;
                        } else {
                            perror("Sending data failed");
                            close(epfd);
                            close(sockfd);
                            exit(EXIT_FAILURE);
                        }
                    }
                    packets_sent++;
                    bytes_sent += sent;
                }
            }
        }

        close(epfd);
    } else {
        struct pollfd pfd = {.fd = sockfd, .events = POLLOUT};

        while (packets_sent < config.workload_size) {
            int poll_res = poll(&pfd, 1, -1);
            if (poll_res == -1) {
                perror("Poll failed");
                close(sockfd);
                exit(EXIT_FAILURE);
            }

            if (pfd.revents & POLLOUT) {
                ssize_t sent = send(sockfd, msg_buffer, BUF_LEN, 0);
                if (sent == -1) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        continue;
                    } else {
                        perror("Sending data failed");
                        close(sockfd);
                        exit(EXIT_FAILURE);
                    }
                }
                packets_sent++;
                bytes_sent += sent;
            }
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t_data_end);
    double data_duration = (t_data_end.tv_sec - t_data_start.tv_sec) * 1000.0 +
                           (t_data_end.tv_nsec - t_data_start.tv_nsec) / 1e6;

    struct timespec t_close_start, t_close_end;
    clock_gettime(CLOCK_MONOTONIC, &t_close_start);

    close(sockfd);

    clock_gettime(CLOCK_MONOTONIC, &t_close_end);
    double close_duration = (t_close_end.tv_sec - t_close_start.tv_sec) * 1000.0 +
                            (t_close_end.tv_nsec - t_close_start.tv_nsec) / 1e6;

    printf("connection open: %.2f ms\n", conn_duration);
    //printf("packets: %d\n", packets_sent);
    printf("performance: %.2f packets/s, %.2f MB/sec\n",
           (packets_sent / (data_duration / 1000.0)),
           (bytes_sent / (data_duration / 1000.0)) / (1024 * 1024));
    printf("connection close: %.2f ms\n", close_duration);

    return 0;
}
