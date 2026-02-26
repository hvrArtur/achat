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
#include <chrono>
#include <threadPool.h>
#pragma comment (lib, "Ws2_32.lib")

using namespace std;
#define DEFAULT_TIMEOUT 10000
#define TEXT_MSG 10
#define CONN_TIMEOUT 2
#define CONN_ERROR 1
#define CONN_CLOSED 0

struct MSG {
    string senderId;
    string senderUsername;
    string data;
    uint8_t command;
    uint32_t timestamp;
};

bool detectError (int res, shared_ptr<MSG> msg){
    msg->timestamp = chrono::duration_cast<chrono::seconds>(chrono::system_clock::now().time_since_epoch()).count();
    if (res < 0) {
        msg->command = CONN_ERROR;
        msg->data = to_string(WSAGetLastError());
        return true;
    }
    if (res == 2) {
        msg->command = CONN_TIMEOUT;
        msg->data = to_string(WSAGetLastError());
        return true;
    }
    if (res == 0) {
        msg->command = CONN_CLOSED;
        msg->data = "CONN_CLOSED";
        return true;
    }
    return false;
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
        if(listenThread.joinable()) listenThread.join();
    }

    bool sendMsg(shared_ptr<MSG> msg){
        lock_guard<mutex> lk(sendM);
        uint32_t n = static_cast<uint32_t>(msg->data.size());
        n = (n << 8) | msg->command;
        int sendres = send(clientSocket, (char*)n, 4, 0 );
        if (sendres == SOCKET_ERROR) {
            cout << "Send failed with error: " << WSAGetLastError() << endl;
            return false;
        }

        sendres = send(clientSocket, (char*)msg->timestamp, 4, 0 );
        if (sendres == SOCKET_ERROR) {
            cout << "Send failed with error: " << WSAGetLastError() << endl;
            return false;
        }

        sendres = send(clientSocket, msg->senderId.data(), 16, 0 );
        if (sendres == SOCKET_ERROR) {
            cout << "Send failed with error: " << WSAGetLastError() << endl;
            return false;
        }

        sendres = send(clientSocket, msg->senderUsername.data(), 24, 0 );
        if (sendres == SOCKET_ERROR) {
            cout << "Send failed with error: " << WSAGetLastError() << endl;
            return false;
        }

        sendres = send(clientSocket, msg->data.data(), msg->data.size(), 0 );
        if (sendres == SOCKET_ERROR) {
            cout << "Send failed with error: " << WSAGetLastError() << endl;
            return false;
        }

        return true;
    }

    string username;
    string id;

private:
    void listenSocket(){
        while (true) {
            {
                unique_lock<mutex> lock(listneningM);
                listneningVar.wait(lock, [this] { return !listneningAA || listneningRN; });

                if (!listneningAA) break;
            }
            shared_ptr<MSG> msg = make_shared<MSG>();
            msg->senderUsername.resize(15);
            msg->senderId = id;
            msg->senderUsername.resize(24);
            msg->senderUsername = username;
            bool res = recv_msg(msg);
            {
                unique_lock<mutex> lock(listneningM);
                if (!listneningAA) break;
            }
            if(res){
                if(msg->command == TEXT_MSG){
                    auto hub = owner;
                    auto uid = id;
                    hub->pool.post([hub, msg, uid] {
                        hub->sendToAll(msg, uid);
                    });
                    continue;
                }else{
                    cout<< "While listnening got unknown command: [" << msg->command << "] at "<< msg->timestamp << endl;
                    break;
                }
            } else if(msg->command == CONN_ERROR) {
                cout<< "While listnening got error: [" << msg->data << "] at "<< msg->timestamp << endl;
                break;
            } else if(msg->command == CONN_TIMEOUT){
                continue;
            } else {
                cout<< "While listnening got unknown command: [" << msg->command << "] at "<< msg->timestamp << endl;
                break;
            }
        }
        auto hub = owner;
        auto uid = id;

        hub->pool.post([hub, uid] {
            hub->disconnectUser(uid);
        });
    }

    int recv_all(char* data, int len) {
        int got = 0;
        while (got < len) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(clientSocket, &rfds);

            timeval tv;
            tv.tv_sec  = DEFAULT_TIMEOUT / 1000;
            tv.tv_usec = (DEFAULT_TIMEOUT % 1000) * 1000;

            int sel = select(0, &rfds, nullptr, nullptr, &tv);
            if (sel == 0) return CONN_TIMEOUT;
            if (sel < 0) return CONN_ERROR;

            int rc = recv(clientSocket, data + got, len - got, 0);
            if (rc <= 0) return rc;
            got += rc;
        }
        return 3;
    }

    bool recv_msg(shared_ptr<MSG> msg) {
        uint32_t n;
        int res = recv_all((char*)&n, 4);
        bool err = detectError(res, msg);
        if(err) return err;
        n = ntohl(n);

        uint32_t bytes = n >> 8;
        uint8_t command = n & 0xFF;
        
        msg->data.resize(bytes);
        msg->command = command;
        
        res = recv_all(msg->data.data(), (int)bytes);
        err = detectError(res, msg);
        if(err) return err;
        return true;
    }

    mutex listneningM;
    bool listneningAA{true};
    bool listneningRN{true};
    condition_variable listneningVar;
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

    void disconnectUser(string id){
        unique_lock<shared_mutex> lk(usersM);
        users.erase(id);
    }

    void sendToAll(shared_ptr<MSG> msg, string exception){
        usersM.lock();
        auto snapshot = make_unique<shared_ptr<USER>[]>(users.size());
        size_t len = 0;
        for (auto& [id, user] : users) {
            if (id == exception) continue;
            snapshot[len++] = user;
        }
        usersM.unlock();

        for(size_t i = 0; i<len; i++){
            snapshot[i]->sendMsg(msg);
        }
        return;
    }

    THREAD_POOL pool;

private:
    void acceptNewSockets() {
        while (true) {
            {
                unique_lock<mutex> lock(acceptingMutex);
                acceptingVar.wait(lock, [this] { return !acceptingAA || acceptingRN; });

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
    condition_variable acceptingVar;
    thread acceptingThread;
    SOCKET acceptingSocket;
    shared_mutex usersM;
    unordered_map<string, shared_ptr<USER>> users;
};