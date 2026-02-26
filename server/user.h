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
#include <string>
#include <condition_variable>
#include <threadPool.h>
#pragma comment (lib, "Ws2_32.lib")

using namespace std;
#define DEFAULT_TIMEOUT 10000

struct MSG {
    string data;
    uint8_t command;
};

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

bool recv_msg(SOCKET s, MSG& msg) {
    uint16_t bytes;
    if (!recv_all(s, (char*)&bytes, 2)) return false;
    bytes = ntohl(bytes);
    
    uint8_t command;
    if (!recv_all(s, (char*)&command, 1)) return false;
    command = ntohl(command);
    
    msg.data.resize(bytes);
    msg.command = command;
    return recv_all(s, msg.data.data(), (int)bytes);
}

class USER{
public:
    USER(SOCKET newSocket, HUB* owner) : owner(owner){
        sockaddr_in addr;
        int addrLen = sizeof(addr);

        if (getpeername(newSocket, (sockaddr*)&addr, &addrLen) != 0) {
            throw runtime_error("getpeername failed: " + to_string(WSAGetLastError()));
        }

        char ipStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ipStr, sizeof(ipStr));
        id = string(ipStr) + ":" + to_string(ntohs(addr.sin_port));

        clientSocket = newSocket;

        listenThread = thread(&USER::listenSocket, this);
    }
    ~USER(){
        closesocket(clientSocket);
        listenThread.join();
    }

    string username;
    string id;

private:
    void listenSocket(){
        // Here will be function to listen to socket
    }

    HUB* owner;
    mutex sendM;
    SOCKET clientSocket;
    thread listenThread;
};

class HUB{
public:
    HUB(const char* port, size_t workerAmount) : pool(workerAmount){
        int acres;
        acceptingSocket = INVALID_SOCKET;

        struct addrinfo *result = NULL;
        struct addrinfo hints;

        ZeroMemory(&hints, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        hints.ai_flags = AI_PASSIVE;

        acres = getaddrinfo(NULL, port, &hints, &result);
        if (acres != 0) throw runtime_error("getaddrinfo failed with error: " + to_string(acres));

        acceptingSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
        if (acceptingSocket == INVALID_SOCKET) {
            freeaddrinfo(result);
            throw runtime_error("socket failed with error: " + to_string(WSAGetLastError()));
        }

        acres = bind(acceptingSocket, result->ai_addr, (int)result->ai_addrlen);
        if (acres == SOCKET_ERROR) {
            freeaddrinfo(result);
            closesocket(acceptingSocket);
            throw runtime_error("bind failed with error: " + to_string(WSAGetLastError()));
        }

        freeaddrinfo(result);

        acres = listen(acceptingSocket, SOMAXCONN);
        if (acres == SOCKET_ERROR) {
            closesocket(acceptingSocket);
            throw runtime_error("listen failed with error: " + to_string(WSAGetLastError()));
        }

        acceptingThread = thread(&HUB::acceptNewSockets, this);
    }

    ~HUB(){
        
        {
            lock_guard<mutex> lk(acceptingMutex);
            acceptingAA = false;
        }
        acceptingVar.notify_all();

        if (acceptingThread.joinable()) acceptingThread.join();
        if (acceptingSocket != INVALID_SOCKET) {
            closesocket(acceptingSocket);
            acceptingSocket = INVALID_SOCKET;
        }
    }

    void startAccepting(){ 
        {
            lock_guard<mutex> lk(acceptingMutex);
            acceptingRN = true;
        }
        acceptingVar.notify_all();
    }
    void stopAccepting(){ 
        lock_guard<mutex> lk(acceptingMutex);
        acceptingRN = false;
    }

    THREAD_POOL pool;
    SOCKET acceptingSocket;
    shared_mutex usersM;
    unordered_map<string, shared_ptr<USER>> users;

private:
    void acceptNewSockets() {
        while (true) {
            {
                unique_lock<mutex> lock(acceptingMutex);
                acceptingVar.wait(lock, [this] {
                    return !acceptingAA || acceptingRN;
                });

                if (!acceptingAA) break;
            }
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(acceptingSocket, &readfds);

            timeval tv{};
            tv.tv_sec = 1;

            int r = select(0, &readfds, nullptr, nullptr, &tv);
            {
                lock_guard<mutex> lk(acceptingMutex);
                if (!acceptingAA) break;
                if (!acceptingRN) continue;
            }

            if (r == SOCKET_ERROR) {
                cout << "select error: " << WSAGetLastError() << "\n";
                continue;
            }
            if (r == 0) continue;

            if (FD_ISSET(acceptingSocket, &readfds)) {
                SOCKET connectedSocket = accept(acceptingSocket, NULL, NULL);
                if (connectedSocket == INVALID_SOCKET) {
                    cout << "accept error: " << WSAGetLastError() << "\n";
                    continue;
                }

                try { 
                    shared_ptr<USER> newUser = make_shared<USER>(connectedSocket, this);
                    usersM.lock();
                    users[newUser->id] = newUser;
                    usersM.unlock();
                    cout<<"Accepted: "<<newUser->id<<endl;
                }
                catch(const exception& e){
                    cerr << "GetId error: " << e.what() << "        ";
                    cerr << "Type: " << typeid(e).name() << endl;
                    closesocket(connectedSocket);
                    continue;
                }
            }
        }
    }

    mutex acceptingMutex;
    bool acceptingAA{true};
    bool acceptingRN{false};
    thread acceptingThread;
    condition_variable acceptingVar;
};


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