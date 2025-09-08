#Include
INCLUDE_DIRECTORIES(${PROJECT_SOURCE_DIR}/Include/)

# Variables
SET(CMAKE_CXX_STANDARD 17)
SET(SOURCES
ChatClient.cpp
ChatClient.h

Client.cpp
)

#Exe
ADD_EXECUTABLE(Client ${SOURCES})