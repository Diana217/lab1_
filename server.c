#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <poll.h>

#define BUF_SIZE 1024
#define UNIX_SOCK_PATH "/tmp/perf_test_socket"
#define MAX_EVENTS 10

typedef enum {
    SOCK_UNIX, SOCK_INET
} sock_type;

typedef enum {
    MODE_BLOCKING_SYNC, MODE_NONBLOCKING_SYNC,
    MODE_BLOCKING_ASYNC, MODE_NONBLOCKING_ASYNC
} sock_mode;

typedef struct {
    int fd;
    int packets_received;
} client_info_t;

typedef struct {
    sock_type type;
    sock_mode mode;
    int port;
} server_config;

void parse_args(int argc, char *argv[], server_config *config) {
    static struct option long_options[] = {
        {"type", required_argument, 0, 't'},
        {"mode", required_argument, 0, 'm'},
        {"port", required_argument, 0, 'p'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "t:m:p:", long_options, NULL)) != -1) {
        switch (opt) {
            case 't':
                if (strcmp(optarg, "unix") == 0)
                    config->type = SOCK_UNIX;
                else if (strcmp(optarg, "inet") == 0)
                    config->type = SOCK_INET;
                else {
                    fprintf(stderr, "Unsupported socket type: %s\n", optarg);
                    exit(EXIT_FAILURE);
                }
                break;
            case 'm':
                if (strcmp(optarg, "blocking-sync") == 0)
                    config->mode = MODE_BLOCKING_SYNC;
                else if (strcmp(optarg, "nonblocking-sync") == 0)
                    config->mode = MODE_NONBLOCKING_SYNC;
                else if (strcmp(optarg, "blocking-async") == 0)
                    config->mode = MODE_BLOCKING_ASYNC;
                else if (strcmp(optarg, "nonblocking-async") == 0)
                    config->mode = MODE_NONBLOCKING_ASYNC;
                else {
                    fprintf(stderr, "Invalid mode: %s\n", optarg);
                    exit(EXIT_FAILURE);
                }
                break;
            case 'p':
                config->port = atoi(optarg);
                break;
            default:
                fprintf(stderr, "Usage: %s --type [unix|inet] --mode [blocking-sync|nonblocking-sync|blocking-async|nonblocking-async]\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }
}

int setup_server_socket(server_config *config, struct sockaddr_un *unix_addr, struct sockaddr_in *inet_addr) {
    int server_fd;

    if (config->type == SOCK_UNIX) {
        server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (server_fd == -1) {
            perror("socket");
            exit(EXIT_FAILURE);
        }

        unlink(UNIX_SOCK_PATH);

        memset(unix_addr, 0, sizeof(*unix_addr));
        unix_addr->sun_family = AF_UNIX;
        strncpy(unix_addr->sun_path, UNIX_SOCK_PATH, sizeof(unix_addr->sun_path) - 1);

        if (bind(server_fd, (struct sockaddr *)unix_addr, sizeof(*unix_addr)) == -1) {
            perror("bind");
            close(server_fd);
            exit(EXIT_FAILURE);
        }
    } else {
        server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd == -1) {
            perror("socket");
            exit(EXIT_FAILURE);
        }

        int opt = 1;
        if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
            perror("setsockopt");
            close(server_fd);
            exit(EXIT_FAILURE);
        }

        memset(inet_addr, 0, sizeof(*inet_addr));
        inet_addr->sin_family = AF_INET;
        inet_addr->sin_addr.s_addr = INADDR_ANY;
        inet_addr->sin_port = htons(config->port);

        if (bind(server_fd, (struct sockaddr *)inet_addr, sizeof(*inet_addr)) == -1) {
            perror("bind");
            close(server_fd);
            exit(EXIT_FAILURE);
        }
    }

    return server_fd;
}

void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl get");
        close(fd);
        exit(EXIT_FAILURE);
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl set");
        close(fd);
        exit(EXIT_FAILURE);
    }
}

int main(int argc, char *argv[]) {
    server_config config;
    config.type = SOCK_UNIX;
    config.mode = MODE_BLOCKING_SYNC;
    config.port = 8080;

    parse_args(argc, argv, &config);

    int server_fd;
    struct sockaddr_un unix_addr;
    struct sockaddr_in inet_addr;

    server_fd = setup_server_socket(&config, &unix_addr, &inet_addr);

    if (config.mode == MODE_NONBLOCKING_SYNC || config.mode == MODE_NONBLOCKING_ASYNC) {
        set_nonblocking(server_fd);
    }

    if (listen(server_fd, 10) == -1) {
        perror("listen");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Server is listening (%s mode)...\n",
           (config.mode == MODE_BLOCKING_SYNC || config.mode == MODE_BLOCKING_ASYNC) ? "blocking" : "unblocking");

    if (config.mode == MODE_BLOCKING_ASYNC || config.mode == MODE_NONBLOCKING_ASYNC) {
        int epoll_fd = epoll_create1(0);
        if (epoll_fd == -1) {
            perror("epoll_create1");
            close(server_fd);
            exit(EXIT_FAILURE);
        }

        struct epoll_event event;
        event.events = EPOLLIN;
        event.data.ptr = NULL;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) == -1) {
            perror("epoll_ctl");
            close(server_fd);
            close(epoll_fd);
            exit(EXIT_FAILURE);
        }

        struct epoll_event events_list[MAX_EVENTS];

        while (1) {
            int n = epoll_wait(epoll_fd, events_list, MAX_EVENTS, -1);
            if (n == -1) {
                perror("epoll_wait");
                close(server_fd);
                close(epoll_fd);
                exit(EXIT_FAILURE);
            }

            for (int i = 0; i < n; i++) {
                if (events_list[i].data.ptr == NULL) {
                    int client_fd = accept(server_fd, NULL, NULL);
                    if (client_fd == -1) {
                        perror("accept");
                        continue;
                    }

                    printf("New client connected\n");

                    if (config.mode == MODE_NONBLOCKING_SYNC || config.mode == MODE_NONBLOCKING_ASYNC) {
                        set_nonblocking(client_fd);
                    }

                    client_info_t *client_info = malloc(sizeof(client_info_t));
                    if (client_info == NULL) {
                        perror("malloc");
                        close(client_fd);
                        continue;
                    }
                    client_info->fd = client_fd;
                    client_info->packets_received = 0;

                    struct epoll_event client_event;
                    client_event.events = EPOLLIN | EPOLLET;
                    client_event.data.ptr = client_info;
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_event) == -1) {
                        perror("epoll_ctl: client_fd");
                        close(client_fd);
                        free(client_info);
                        continue;
                    }
                } else {
                    client_info_t *client_info = (client_info_t *)events_list[i].data.ptr;
                    int client_fd = client_info->fd;
                    char buf[BUF_SIZE];
                    ssize_t bytes_received;

                    while ((bytes_received = recv(client_fd, buf, sizeof(buf), 0)) > 0) {
                        client_info->packets_received++;
                    }

                    if (bytes_received == 0) {
                        printf("Client disconnected. Received packages: %d\n", client_info->packets_received);
                        close(client_fd);
                        free(client_info);
                    } else if (bytes_received == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        perror("recv");
                        close(client_fd);
                        free(client_info);
                    }
                }
            }
        }

        close(epoll_fd);
    } else {
        struct pollfd fds[1];
        fds[0].fd = server_fd;
        fds[0].events = POLLIN;

        while (1) {
            int ret = poll(fds, 1, -1);
            if (ret == -1) {
                perror("poll");
                close(server_fd);
                exit(EXIT_FAILURE);
            }

            if (fds[0].revents & POLLIN) {
                int client_fd = accept(server_fd, NULL, NULL);
                if (client_fd == -1) {
                    perror("accept");
                    continue;
                }

                printf("New client connected\n");

                if (config.mode == MODE_NONBLOCKING_SYNC) {
                    set_nonblocking(client_fd);
                }

                struct pollfd client_fds[1];
                client_fds[0].fd = client_fd;
                client_fds[0].events = POLLIN;
                int packets_received = 0;

                while (1) {
                    int client_ret = poll(client_fds, 1, -1);
                    if (client_ret == -1) {
                        perror("poll: client");
                        close(client_fd);
                        break;
                    }

                    if (client_fds[0].revents & POLLIN) {
                        char buf[BUF_SIZE];
                        ssize_t bytes = recv(client_fd, buf, sizeof(buf), 0);
                        if (bytes > 0) {
                            packets_received++;
                        } else if (bytes == 0) {
                            printf("Client disconnected. Received packages: %d\n", packets_received);
                            close(client_fd);
                            break;
                        } else {
                            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                                perror("recv");
                                close(client_fd);
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    close(server_fd);
    return 0;
}
