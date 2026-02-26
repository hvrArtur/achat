#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <utility>
#include <vector>

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

private:
    void killWorkers(){
        {
            unique_lock<mutex> lock(m);
            stopAA = true;
        }
        cv.notify_all();
        for(size_t i = 0; i<workers.size(); i++) {
            if(workers[i].workingThread.joinable()) workers[i].workingThread.join();
        }
    }
    vector<WORKER> workers;
};

class WORKER {
public:

    explicit WORKER(THREAD_POOL* owner) : owner(owner), workingThread([this]{ run(); }) {}
    thread workingThread;

private:

    void run() {
        while (true) {
            function<void()> task;

            {
                unique_lock<mutex> lock(owner->m);
                owner->cv.wait(lock, [this] { return owner->stopAA || (!owner->stopRN && !owner->tasks.empty()); });

                if (owner->stopAA) break;

                task = std::move(owner->tasks.front());
                owner->tasks.pop();
            }

            task();
        }
    }

    THREAD_POOL* owner;
    function<void()> task;
};