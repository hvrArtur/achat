#undef UNICODE

#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iostream>
#include <thread>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <string>
#pragma comment (lib, "Ws2_32.lib")

using namespace std;
#define DEFAULT_TIMEOUT 10000
#define DEFAULT_BUFLEN 511
#define DEFAULT_SYSTEM_BUFLEN 3
#define DEFAULT_PORT "27000"

class USER{
    public:
        SOCKET socket;
        string username;
        mutex sendM;
    ~USER(){
        closesocket(socket);
    }
};

class HUB{
public:
    SOCKET acceptingSocket;
    shared_mutex usersM;
    unordered_map<string, shared_ptr<USER>> users;

    HUB(int port){
        int acres;
        acceptingSocket = INVALID_SOCKET;

        struct addrinfo *result = NULL;
        struct addrinfo hints;

        ZeroMemory(&hints, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        hints.ai_flags = AI_PASSIVE;

        acres = getaddrinfo(NULL, DEFAULT_PORT, &hints, &result);
        if (acres != 0) throw runtime_error("getaddrinfo failed with error: " + acres);

        acceptingSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
        if (acceptingSocket == INVALID_SOCKET) {
            freeaddrinfo(result);
            throw runtime_error("socket failed with error: " + WSAGetLastError());
        }

        acres = bind(acceptingSocket, result->ai_addr, (int)result->ai_addrlen);
        if (acres == SOCKET_ERROR) {
            freeaddrinfo(result);
            closesocket(acceptingSocket);
            throw runtime_error("bind failed with error: " + WSAGetLastError());
        }

        freeaddrinfo(result);

        acres = listen(acceptingSocket, SOMAXCONN);
        if (acres == SOCKET_ERROR) {
            closesocket(acceptingSocket);
            throw runtime_error("listen failed with error: " + WSAGetLastError());
        }
    }
};


mutex usersM;
unordered_map<string, shared_ptr<USER>> users;

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

bool recv_all(SOCKET s, char* data, int len) {
    int got = 0;
    while (got < len) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s, &rfds);

        timeval tv;
        tv.tv_sec  = DEFAULT_TIMEOUT / 1000;
        tv.tv_usec = (DEFAULT_TIMEOUT % 1000) * 1000;

        int sel = select(0, &rfds, nullptr, nullptr, &tv);
        if (sel == 0) return false;
        if (sel < 0)  return false;

        int rc = recv(s, data + got, len - got, 0);
        if (rc <= 0) return false;
        got += rc;
    }
    return true;
}

int recv_msg(SOCKET s, char* data) {
    char bytesc[DEFAULT_SYSTEM_BUFLEN+1];
    if (!recv_all(s, bytesc, DEFAULT_SYSTEM_BUFLEN)) return false;
    
    
    if (n == 0) return true;
    return recv_all(s, out.data(), (int)n);
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
        shared_ptr<USER> newUser = make_shared<USER>();
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