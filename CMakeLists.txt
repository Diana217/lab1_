cmake_minimum_required(VERSION 3.29)
project(sockets C)

set(CMAKE_C_STANDARD 23)

set(CLIENT_SOURCE client.c)

set(SERVER_SOURCE server.c)

add_executable(client ${CLIENT_SOURCE})
add_executable(server ${SERVER_SOURCE})