#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <utility>
#include <vector>
#include <iostream>

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
                    std::function<void()> task;

                    {
                        std::unique_lock<std::mutex> lock(owner->m);
                        owner->cv.wait(lock, [this] { return !owner->living || (owner->working && !owner->tasks.empty()); });

                        if (!owner->living) break;

                        task = std::move(owner->tasks.front());
                        owner->tasks.pop();
                    }
                    try{
                        task();
                    }catch(const std::exception& e){
                        std::cerr << "Error while doing task: " << e.what() << std::endl;
                        std::cerr << "Type: " << typeid(e).name() << std::endl;
                    }catch(...){
                        std::cerr << "Uknown errro while doing task" << std::endl;
                    }
                }
            }

            THREAD_POOL* owner;
            std::function<void()> task;

        public:
            std::thread workingThread;
    };

    void post(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(m);
            if(!living) return;
            tasks.push(std::move(task));
        }
        cv.notify_one();
    }

    void stopPool() {
        {
            std::lock_guard<std::mutex> lock(m);
            if (!working) return;
            working = false;
        }
        cv.notify_all();
    }

    void startPool() {
        {
            std::lock_guard<std::mutex> lock(m);
            if (working) return;
            working = true;
        }
        cv.notify_all();
    }

    std::mutex m;
    std::condition_variable cv;
    bool working{true};
    bool living{true};
    std::queue<std::function<void()>> tasks;

    void killWorkers(){
        {
            std::unique_lock<std::mutex> lock(m);
            living = false;
        }
        cv.notify_all();
        while (!workers.empty()) {
            auto& w = workers.back();
            if (w.workingThread.joinable()) w.workingThread.join();
            workers.pop_back();
        }
    }

private:
    std::vector<WORKER> workers;
};