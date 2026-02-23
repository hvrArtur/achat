#undef UNICODE

#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <thread>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <string>
#pragma comment (lib, "Ws2_32.lib")

using namespace std;
#define DEFAULT_BUFLEN 511
#define DEFAULT_SYSTEM_BUFLEN 3
#define DEFAULT_PORT "27000"

struct USER{
    SOCKET socket;
    mutex sendM;
};

mutex usersM;
unordered_map<string, shared_ptr<USER>> users;

SOCKET createListneningSocket (){
    int acres;

    SOCKET ListenSocket = INVALID_SOCKET;

    struct addrinfo *result = NULL;
    struct addrinfo hints;

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    acres = getaddrinfo(NULL, DEFAULT_PORT, &hints, &result);
    if (acres != 0) {
        cout << "getaddrinfo failed with error: " << acres << endl;
        WSACleanup();
        return INVALID_SOCKET;
    }

    // Create a SOCKET for the server to listen for client connections.
    ListenSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (ListenSocket == INVALID_SOCKET) {
        cout << "socket failed with error: " << WSAGetLastError() << endl;
        freeaddrinfo(result);
        WSACleanup();
        return INVALID_SOCKET;
    }

    acres = bind(ListenSocket, result->ai_addr, (int)result->ai_addrlen);
    if (acres == SOCKET_ERROR) {
        cout << "bind failed with error: "<< WSAGetLastError() << endl;
        freeaddrinfo(result);
        closesocket(ListenSocket);
        WSACleanup();
        return INVALID_SOCKET;
    }

    freeaddrinfo(result);

    acres = listen(ListenSocket, SOMAXCONN);
    if (acres == SOCKET_ERROR) {
        cout << "listen failed with error: "
            << WSAGetLastError() << endl;
        closesocket(ListenSocket);
        WSACleanup();
        return INVALID_SOCKET;
    }

        return ListenSocket;
    }

bool recv_all(SOCKET s, char* data, int len) {
    int got = 0;
    while (got < len) {
        int rc = recv(s, data + got, len - got, 0);
        if (rc <= 0) return false;
        got += rc;
    }
    return true;
}

bool recv_msg(SOCKET s, std::string& out) {
    uint32_t n_net;
    if (!recv_all(s, (char*)&n_net, 4)) return false;
    uint32_t n = ntohl(n_net);

    out.resize(n);
    if (n == 0) return true;
    return recv_all(s, out.data(), (int)n);
}

int getId(string& id, SOCKET socket){
    sockaddr_in addr;
    int addrLen = sizeof(addr);

    if (getpeername(socket, (sockaddr*)&addr, &addrLen) != 0) {
        cout << "getpeername failed: " << WSAGetLastError() << endl;
        closesocket(socket);
        return 1;
    }

    char ipStr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, ipStr, sizeof(ipStr));
    id = string(ipStr) + ":" + to_string(ntohs(addr.sin_port));

    return 0;
}

void sendToAll(char* recvbuf, int acres, string exception){
    usersM.lock();
    auto snapshot = make_unique<shared_ptr<USER>[]>(users.size());
    size_t len = 0;
    for (auto& [id, user] : users) {
        if (id == exception) continue;
        snapshot[len++] = user;
    }
    usersM.unlock();

    for(size_t i = 0; i<len; i++){
        snapshot[i]->sendM.lock();
        string id;
        getId(id, snapshot[i]->socket);
        int sendres = send( snapshot[i]->socket, recvbuf, acres, 0 );
        cout << "Sending ["<< recvbuf << "] with length: "<<acres << endl;
        if (sendres == SOCKET_ERROR) {
            cout << "Send failed with error: " << WSAGetLastError() << endl;
        }
        snapshot[i]->sendM.unlock();
    }
    delete[] recvbuf;
    return;
}

void listenSocket(shared_ptr<USER> user, string id){
    int acres = 0;
    char bytesc[DEFAULT_SYSTEM_BUFLEN+1];
    char commandc[DEFAULT_SYSTEM_BUFLEN+1];
    int bytes;
    int command;
    do {
        bytes = 0;
        command = 0;
        char* recvbuf = new char[DEFAULT_BUFLEN+1 ];

        acres = recv(user->socket, bytesc, DEFAULT_SYSTEM_BUFLEN, 0);
        if(acres < 0){
            delete[] recvbuf;
            cout << "Recv failed with error: " << WSAGetLastError() << " From: " << id << endl;
        } else if(acres == 0){
            delete[] recvbuf;
            break;
        }
        bytesc[DEFAULT_SYSTEM_BUFLEN] = '\0';

        acres = recv(user->socket, commandc, DEFAULT_SYSTEM_BUFLEN, 0);
        if(acres < 0){
            delete[] recvbuf;
            cout << "Recv failed with error: " << WSAGetLastError() << " From: " << id << endl;
        } else if(acres == 0){
            delete[] recvbuf;
            break;
        }
        commandc[DEFAULT_SYSTEM_BUFLEN] = '\0';

        acres = recv(user->socket, bytesc, DEFAULT_BUFLEN, 0);
        if(acres < 0){
            delete[] recvbuf;
            cout << "Recv failed with error: " << WSAGetLastError() << " From: " << id << endl;
        } else if(acres == 0){
            delete[] recvbuf;
            break;
        }
        recvbuf[acres] = '\0';
        cout << "Received: [" << recvbuf << "] From: " << id << endl;
        bytes = atoi(bytesc);
        if(bytes = 0){

        }
        sendToAll(recvbuf, bytes, id);

    } while (true);

    cout << "Closing connection with: " << id << endl;
    closesocket(user->socket);
    usersM.lock();
    users.erase(id);
    usersM.unlock();
    return;
}

void acceptNewSockets (SOCKET listeningSocket){
    while(true){
        SOCKET connectedSocket = INVALID_SOCKET;

        connectedSocket = accept(listeningSocket, NULL, NULL);
        if (connectedSocket == INVALID_SOCKET) {
            cout << "Accept failed with error: " << WSAGetLastError() << endl;
            continue;
        }

        string id;
        if(getId(id, connectedSocket)) continue;
        auto newUser = make_shared<USER>();
        newUser->socket=connectedSocket;
        usersM.lock();
        users[id] = newUser;
        usersM.unlock();
        thread t(listenSocket, newUser, id);
        t.detach();
        cout<<"Accepted: "<<id<<endl;
    }
}

int main()
{
    WSADATA wsaData;
    int acres;

    struct addrinfo *result = NULL;
    struct addrinfo hints;

    int iSendResult;
    char recvbuf[DEFAULT_BUFLEN];
    int recvbuflen = DEFAULT_BUFLEN;
    
    acres = WSAStartup(MAKEWORD(2,2), &wsaData);
    if (acres != 0) {
        cout << "WSAStartup error: " << acres << endl;
        return 1;
    }

    SOCKET listeningSocket = createListneningSocket();
    if(listeningSocket == INVALID_SOCKET){
        cout << "Creation of listnening socket failed: " << WSAGetLastError() << endl;
        WSACleanup();
        return 1;
    }
    
    thread t(acceptNewSockets, listeningSocket);
    t.join();
    closesocket(listeningSocket);
    WSACleanup();

    return 0;
}