#Include
INCLUDE_DIRECTORIES(${PROJECT_SOURCE_DIR}/Include/)

# Variables
SET(CMAKE_CXX_STANDARD 17)
SET(SOURCES
ClientSession.cpp
ClientSession.h

ChatServer.cpp
ChatServer.h

Server.cpp
)

#Exe
ADD_EXECUTABLE(Server Server.cpp)