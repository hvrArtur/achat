cmake_minimum_required(VERSION 3.16)
project(achat_all)

add_subdirectory(client)
add_subdirectory(server)

add_custom_target(buildAll DEPENDS achatclient achatserver)