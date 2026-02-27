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
#define DEFAULT_PAYLOAD_LENGTH 1024
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

class HUB;

class USER{
public:
    USER(SOCKET newSocket, HUB* owner) : owner(owner){
        sockaddr_in addr;
        int addrLen = sizeof(addr);

        if (getpeername(newSocket, (sockaddr*)&addr, &addrLen) != 0) {
            throw runtime_error("getpeername failed: " + to_string(WSAGetLastError()));
        }

        char ipStr[INET_ADDRSTRLEN];
        if(!inet_ntop(AF_INET, &addr.sin_addr, ipStr, sizeof(ipStr))) throw runtime_error("inet_ntop failed: " + to_string(WSAGetLastError()));;
        id = string(ipStr) + ":" + to_string(ntohs(addr.sin_port));

        clientSocket = newSocket;

        listenThread = thread(&USER::listenSocket, this);
    }
    ~USER(){
        {
            lock_guard<mutex>  lk(listneningM);
            bool listneningAA =false;
        }
        listneningVar.notify_all();
        shutdown(clientSocket, SD_BOTH);
        closesocket(clientSocket);
        if(listenThread.joinable()) listenThread.join();
    }

    bool sendTextMsg(shared_ptr<MSG> msg){
        msg->senderId.resize(21, '\0');
        msg->senderUsername.resize(24, '\0');
        lock_guard<mutex> lk(sendM);
        uint32_t header = static_cast<uint32_t>(msg->data.size());
        header = (header << 8) | msg->command;
        header = htonl(header);
        if (!sendAll(reinterpret_cast<char*>(&header), 4)) {
            cout << "Send of header failed with error: " << WSAGetLastError() << endl;
            return false;
        }

        uint32_t ts = htonl(msg->timestamp);
        if(!sendAll(reinterpret_cast<char*>(&ts), sizeof(ts))) {
            cout << "Send of timestamp failed with error: " << WSAGetLastError() << endl;
            return false;
        }

        if (!sendAll(msg->senderId.data(), 21)) {
            cout << "Send of senderId failed with error: " << WSAGetLastError() << endl;
            return false;
        }

        if (!sendAll(msg->senderUsername.data(), 24)) {
            cout << "Send of username failed with error: " << WSAGetLastError() << endl;
            return false;
        }

        if (!sendAll(msg->data.data(), msg->data.size())) {
            cout << "Send of main data with error: " << WSAGetLastError() << endl;
            return false;
        }

        return true;
    }

    void startListnening(){ 
        {
            lock_guard<mutex> lk(listneningM);
            listneningRN = true;
        }
        listneningVar.notify_all();
    }

    void stopListening(){ 
        lock_guard<mutex> lk(listneningM);
        listneningRN = false;
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
            msg->senderId = id;
            msg->senderUsername = username;
            bool res = recvMsg(msg);
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

    bool sendAll(char* data, int len) {
        int sent = 0;
        while (sent < len) {
            int sn = send(clientSocket, data + sent, len - sent, 0);
            if (sn <= 0) return false;
            sent += sn;
        }
        return true;
    }

    bool detectRcvError (int res, shared_ptr<MSG> msg){
        msg->timestamp = chrono::duration_cast<chrono::seconds>(chrono::system_clock::now().time_since_epoch()).count();
        if (res == -1) return false;
        if (res == 2) {
            msg->command = CONN_TIMEOUT;
            msg->data = "CONN_TIMEOUT";
            return true;
        }
        if (res == 1) {
            msg->command = CONN_ERROR;
            msg->data = to_string(WSAGetLastError());
            return true;
        }
        if (res == 0) {
            msg->command = CONN_CLOSED;
            msg->data = "CONN_CLOSED";
            return true;
        }
        msg->command = CONN_ERROR;
        msg->data = to_string(WSAGetLastError());
        return true;
    }

    int recvAll(char* data, int len) {
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
            if (rc < 0) return CONN_ERROR;
            if (rc == 0) return CONN_CLOSED;
            got += rc;
        }
        return -1;
    }

    bool recvMsg(shared_ptr<MSG> msg) {
        uint32_t n;
        int res = recvAll((char*)&n, 4);
        bool err = detectRcvError(res, msg);
        if(err) return false;
        n = ntohl(n);

        uint32_t bytes = n >> 8;
        if(bytes>DEFAULT_PAYLOAD_LENGTH) {
            msg->timestamp = chrono::duration_cast<chrono::seconds>(
                chrono::system_clock::now().time_since_epoch()
            ).count();
            msg->command = CONN_ERROR;
            msg->data = "PAYLOAD_TOO_LARGE";
        };
        uint8_t command = n & 0xFF;
        
        msg->data.resize(bytes);
        msg->command = command;
        
        res = recvAll(msg->data.data(), (int)bytes);
        err = detectRcvError(res, msg);
        if(err) return false;
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

        {
            unique_lock<mutex> lk(pool.m);
            pool.stopAA = true;
        }
        acceptingVar.notify_all();
        if (acceptingThread.joinable()) acceptingThread.join();
        if (acceptingSocket != INVALID_SOCKET) {
            closesocket(acceptingSocket);
            acceptingSocket = INVALID_SOCKET;
        }

        {   
            shared_lock<shared_mutex> lk(usersM);
            for (auto& [id, user] : users) {
                user->stopListening();
            }
        }

        pool.killWorkers();
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
        std::unique_ptr<std::shared_ptr<USER>[]> snapshot;
        size_t len = 0;
        {   
            shared_lock<shared_mutex> lk(usersM);
            snapshot = make_unique<shared_ptr<USER>[]>(users.size());
            for (auto& [id, user] : users) {
                if (id == exception) continue;
                snapshot[len++] = user;
            }
        }
        for(size_t i = 0; i<len; i++){
            snapshot[i]->sendTextMsg(msg);
        }
        return;
    }

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
                    {
                        unique_lock<shared_mutex> lk(usersM);
                        users[newUser->id] = newUser;
                    }
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
public:
    THREAD_POOL pool;
};