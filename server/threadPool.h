#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <utility>
#include <vector>
#include <iostream>

using namespace std;

class THREAD_POOL {
public:
    THREAD_POOL(size_t n) {
        workers.reserve(n);
        for (size_t i = 0; i < n; i++) workers.emplace_back(this);
    }

    ~THREAD_POOL() {
        killWorkers();
    }

    class WORKER {
        public:

            explicit WORKER(THREAD_POOL* owner) : owner(owner), workingThread([this]{ run(); }) {}

        private:

            void run() {
                while (true) {
                    function<void()> task;

                    {
                        unique_lock<mutex> lock(owner->m);
                        owner->cv.wait(lock, [this] { return owner->stopAA || (!owner->stopRN && !owner->tasks.empty()); });

                        if (owner->stopAA) break;

                        task = move(owner->tasks.front());
                        owner->tasks.pop();
                    }
                    try{
                        task();
                    }catch(const exception& e){
                        cerr << "Error while doing task: " << e.what() << endl;
                        cerr << "Type: " << typeid(e).name() << endl;
                    }
                }
            }

            THREAD_POOL* owner;
            function<void()> task;

        public:
            thread workingThread;
    };

    void post(function<void()> task) {
        {
            lock_guard<mutex> lock(m);
            if(stopAA) return;
            tasks.push(move(task));
        }
        cv.notify_one();
    }

    void stopPool() {
        {
            lock_guard<mutex> lock(m);
            if (stopRN) return;
            stopRN = true;
        }
        cv.notify_all();
    }

    void startPool() {
        {
            lock_guard<mutex> lock(m);
            if (!stopRN) return;
            stopRN = false;
        }
        cv.notify_all();
    }

    mutex m;
    condition_variable cv;
    bool stopRN{false};
    bool stopAA{false};
    queue<function<void()>> tasks;

    void killWorkers(){
        {
            unique_lock<mutex> lock(m);
            stopAA = true;
        }
        cv.notify_all();
        while (!workers.empty()) {
            auto& w = workers.back();
            if (w.workingThread.joinable()) w.workingThread.join();
            workers.pop_back();
        }
    }

private:
    vector<WORKER> workers;
};