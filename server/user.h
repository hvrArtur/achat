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

#define DEFAULT_TIMEOUT 10000
#define DEFAULT_PAYLOAD_LENGTH 1024
#define TEXT_MSG 10
#define CONN_TIMEOUT 2
#define CONN_ERROR 1
#define CONN_CLOSED 0

struct MSG {
    std::string senderId;
    std::string senderUsername;
    std::string data;
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
            throw std::runtime_error("getpeername failed: " + std::to_string(WSAGetLastError()));
        }

        char ipStr[INET_ADDRSTRLEN];
        if(!inet_ntop(AF_INET, &addr.sin_addr, ipStr, sizeof(ipStr))) throw std::runtime_error("inet_ntop failed: " + std::to_string(WSAGetLastError()));;
        id = std::string(ipStr) + ":" + std::to_string(ntohs(addr.sin_port));

        clientSocket = newSocket;

        listenThread = std::thread(&USER::listenSocket, this);
    }
    ~USER(){
        {
            std::lock_guard<std::mutex>  lk(listneningM);
            living =false;
        }
        listneningVar.notify_all();
        shutdown(clientSocket, SD_BOTH);
        closesocket(clientSocket);
        if(listenThread.joinable()) listenThread.join();
    }

    bool sendTextMsg(std::shared_ptr<MSG> msg){
        msg->senderId.resize(21, '\0');
        msg->senderUsername.resize(24, '\0');
        std::lock_guard<std::mutex> lk(sendM);
        uint32_t header = static_cast<uint32_t>(msg->data.size());
        header = (header << 8) | msg->command;
        header = htonl(header);
        if (!sendAll(reinterpret_cast<char*>(&header), 4)) {
            std::cout << "Send of header failed with error: " << WSAGetLastError() << std::endl;
            return false;
        }

        uint32_t ts = htonl(msg->timestamp);
        if(!sendAll(reinterpret_cast<char*>(&ts), sizeof(ts))) {
            std::cout << "Send of timestamp failed with error: " << WSAGetLastError() << std::endl;
            return false;
        }

        if (!sendAll(msg->senderId.data(), 21)) {
            std::cout << "Send of senderId failed with error: " << WSAGetLastError() << std::endl;
            return false;
        }

        if (!sendAll(msg->senderUsername.data(), 24)) {
            std::cout << "Send of username failed with error: " << WSAGetLastError() << std::endl;
            return false;
        }

        if (!sendAll(msg->data.data(), msg->data.size())) {
            std::cout << "Send of main data with error: " << WSAGetLastError() << std::endl;
            return false;
        }

        return true;
    }

    void startListnening(){ 
        {
            std::lock_guard<std::mutex> lk(listneningM);
            listnening = true;
        }
        listneningVar.notify_all();
    }

    void stopListening(){ 
        std::lock_guard<std::mutex> lk(listneningM);
        listnening = false;
    }
    
    std::string username;
    std::string id;

private:
        void listenSocket(){
            while (true) {
                {
                    std::unique_lock<std::mutex> lock(listneningM);
                    listneningVar.wait(lock, [this] { return !living || listnening; });

                    if (!living) break;
                }
                std::shared_ptr<MSG> msg = std::make_shared<MSG>();
                msg->senderId = id;
                msg->senderUsername = username;
                bool res = recvMsg(msg);
                {
                    std::unique_lock<std::mutex> lock(listneningM);
                    if (!living) break;
                    if (!listnening) continue;
                }
                if(res){
                    if(msg->command == TEXT_MSG){
                        auto hub = owner;
                        auto uid = id;
                        {
                            std::unique_lock<std::mutex> lock(owner->acceptingMutex);
                            if (!owner->living) return;
                        }
                        hub->pool.post([hub, msg, uid] {
                            hub->sendToAll(msg, uid);
                        });
                        continue;
                    }else{
                        std::cout<< "While listnening got unknown command: [" << msg->command << "] at "<< msg->timestamp << std::endl;
                        break;
                    }
                } else if(msg->command == CONN_ERROR) {
                    std::cout<< "While listnening got error: [" << msg->data << "] at "<< msg->timestamp << std::endl;
                    break;
                } else if(msg->command == CONN_TIMEOUT){
                    continue;
                } else {
                    std::cout<< "While listnening got unknown command: [" << msg->command << "] at "<< msg->timestamp << std::endl;
                    break;
                }
            }
            auto hub = owner;
            auto uid = id;

            {
                std::unique_lock<std::mutex> lock(owner->acceptingMutex);
                if (!owner->living) return;
            }
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

    bool detectRcvError (int res, std::shared_ptr<MSG> msg){
        msg->timestamp = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        if (res == -1) return false;
        if (res == 2) {
            msg->command = CONN_TIMEOUT;
            msg->data = "CONN_TIMEOUT";
            return true;
        }
        if (res == 1) {
            msg->command = CONN_ERROR;
            msg->data = std::to_string(WSAGetLastError());
            return true;
        }
        if (res == 0) {
            msg->command = CONN_CLOSED;
            msg->data = "CONN_CLOSED";
            return true;
        }
        msg->command = CONN_ERROR;
        msg->data = std::to_string(WSAGetLastError());
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

    bool recvMsg(std::shared_ptr<MSG> msg) {
        uint32_t n;
        int res = recvAll((char*)&n, 4);
        bool err = detectRcvError(res, msg);
        if(err) return false;
        n = ntohl(n);

        uint32_t bytes = n >> 8;
        if(bytes>DEFAULT_PAYLOAD_LENGTH) {
            msg->timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
            msg->command = CONN_ERROR;
            msg->data = "PAYLOAD_TOO_LARGE";
            return false;
        };
        uint8_t command = n & 0xFF;
        
        msg->data.resize(bytes);
        msg->command = command;
        
        res = recvAll(msg->data.data(), (int)bytes);
        err = detectRcvError(res, msg);
        if(err) return false;
        return true;
    }

    std::mutex listneningM;
    bool living{true};
    bool listnening{true};
    std::condition_variable listneningVar;
    HUB* owner;
    std::mutex sendM;
    SOCKET clientSocket;
    std::thread listenThread;
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
        if (acres != 0) throw std::runtime_error("getaddrinfo failed with error: " + std::to_string(acres));

        acceptingSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
        if (acceptingSocket == INVALID_SOCKET) {
            freeaddrinfo(result);
            throw std::runtime_error("socket failed with error: " + std::to_string(WSAGetLastError()));
        }

        acres = bind(acceptingSocket, result->ai_addr, (int)result->ai_addrlen);
        if (acres == SOCKET_ERROR) {
            freeaddrinfo(result);
            closesocket(acceptingSocket);
            throw std::runtime_error("bind failed with error: " + std::to_string(WSAGetLastError()));
        }

        freeaddrinfo(result);

        acres = listen(acceptingSocket, SOMAXCONN);
        if (acres == SOCKET_ERROR) {
            closesocket(acceptingSocket);
            throw std::runtime_error("listen failed with error: " + std::to_string(WSAGetLastError()));
        }

        acceptingThread = std::thread(&HUB::acceptNewSockets, this);
    }

    ~HUB(){
        
        {
            std::lock_guard<std::mutex> lk(acceptingMutex);
            living = false;
        }

        {
            std::unique_lock<std::mutex> lk(pool.m);
            pool.living = false;
        }
        acceptingVar.notify_all();
        if (acceptingThread.joinable()) acceptingThread.join();
        if (acceptingSocket != INVALID_SOCKET) {
            closesocket(acceptingSocket);
            acceptingSocket = INVALID_SOCKET;
        }

        {   
            std::shared_lock<std::shared_mutex> lk(usersM);
            for (auto& [id, user] : users) {
                user->stopListening();
            }
        }

        pool.killWorkers();
    }

    void startAccepting(){ 
        {
            std::lock_guard<std::mutex> lk(acceptingMutex);
            accepting = true;
        }
        acceptingVar.notify_all();
    }

    void stopAccepting(){ 
        std::lock_guard<std::mutex> lk(acceptingMutex);
        accepting = false;
    }

    void disconnectUser(std::string id){
        std::unique_lock<std::shared_mutex> lk(usersM);
        users.erase(id);
    }

    void sendToAll(std::shared_ptr<MSG> msg, std::string exception){
        std::unique_ptr<std::shared_ptr<USER>[]> snapshot;
        size_t len = 0;
        {   
            std::shared_lock<std::shared_mutex> lk(usersM);
            snapshot = std::make_unique<std::shared_ptr<USER>[]>(users.size());
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

    std::mutex acceptingMutex;
    bool living{true};

private:
    void acceptNewSockets() {
        while (true) {
            {
                std::unique_lock<std::mutex> lock(acceptingMutex);
                acceptingVar.wait(lock, [this] { return !living || accepting; });

                if (!living) break;
            }
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(acceptingSocket, &readfds);

            timeval tv{};
            tv.tv_sec = 1;

            int r = select(0, &readfds, nullptr, nullptr, &tv);
            {
                std::lock_guard<std::mutex> lk(acceptingMutex);
                if (!living) break;
                if (!accepting) continue;
            }

            if (r == SOCKET_ERROR) {
                std::cout << "select error: " << WSAGetLastError() << "\n";
                continue;
            }
            if (r == 0) continue;

            if (FD_ISSET(acceptingSocket, &readfds)) {
                SOCKET connectedSocket = accept(acceptingSocket, NULL, NULL);
                if (connectedSocket == INVALID_SOCKET) {
                    std::cout << "accept error: " << WSAGetLastError() << "\n";
                    continue;
                }

                try { 
                    std::shared_ptr<USER> newUser = std::make_shared<USER>(connectedSocket, this);
                    {
                        std::unique_lock<std::shared_mutex> lk(usersM);
                        users[newUser->id] = newUser;
                    }
                    std::cout<<"Accepted: "<<newUser->id<<std::endl;
                }
                catch(const std::exception& e){
                    std::cerr << "GetId error: " << e.what() << "        ";
                    std::cerr << "Type: " << typeid(e).name() << std::endl;
                    closesocket(connectedSocket);
                    continue;
                }
            }
        }
    }

    bool accepting{true};
    std::condition_variable acceptingVar;
    std::thread acceptingThread;
    SOCKET acceptingSocket;
    std::shared_mutex usersM;
    std::unordered_map<std::string, std::shared_ptr<USER>> users;
public:
    THREAD_POOL pool;
};