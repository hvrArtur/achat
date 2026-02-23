cmake_minimum_required(VERSION 3.16)

project(achatserver)

add_executable(
  ${PROJECT_NAME}
  main.cpp
)

target_link_libraries(achatserver PRIVATE ws2_32)

install(
  TARGETS ${PROJECT_NAME}
  RUNTIME DESTINATION bin
)