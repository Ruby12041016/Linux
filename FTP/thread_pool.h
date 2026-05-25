// 宏定义防止头文件重复包含
#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

class ThreadPool {
   public:
    // 构造函数，size_t是创建的线程数
    ThreadPool(size_t);
    // 任务队列 /*template<>:写一个模板*/
    template <class F, class... Args> /*claee F:F是要执行的函数类型*/
    auto enqueue(F&& f, Args&&... args) -> std::future<
        typename std::
            result_of</*class...
                         Args：Args是函数的一堆参数类型，...是可变参数模板,表示任意数量的参数*/
                      F(Args...)>::
                type>;  // 尾置限定符，利用result_of来
                        // /*auto:位置返回类型，表示先占个位置，返回值类型后面写*/
    // 推断调用F后返回值类型，并存储在future中 /*enqueue:函数名*/ 注意：C++17 中
    // std::result_of 被弃用，推荐使用 std::invoke_result 替代 /*F&&
    // f:f是要丢进线程池里执行的操作（函数或lambda）*/ 析构函数
    // /*&&:万能引用，传左/右值就接受左/右值，实现完美转发，不拷贝不浪费直接把函数转发进去*/
    ~ThreadPool(); /*Args&&... args:Args...
                      是一堆参数类型，args是这堆参数的统一名字*/
                   /*F(Args...)：用Args...这些参数去调用F*/
   private:        /*std::result_of<F(Args...)>：推断返回值类型,
                      ::type:拿出这个返回值类型*/
    // need to keep track of threads so we can join them
    // /*std::future<返回值类型>：函数在线程池里异步执行，还没执行完，先返回一个未来能拿到结果的对象*/
    std::vector<std::thread>
        workers; /*future的get():阻塞等待任务实现并拿到结果，函数返回值就是结果*/
    // the task queue /*typename:告诉编译器这是一个类型不是变量*/
    std::queue<std::function<void()> > tasks;
        /*定义一个任务变量*/ /*std::function<>：装任意可调用对象（函数、lambda、函数对象、bind过后的东西），不管是什么，只要能
                           加括号调用执行，把不同类型的可调用对象转化成同一个类型，这样就能放进同一个队列里*/

    // synchronization //同步
    std::mutex queue_mutex;             // 互斥锁
    std::condition_variable condition;  // 条件变量
    bool stop;                          // 停止标志
};

// the constructor just launches some amount of workers
inline ThreadPool::ThreadPool(
    size_t threads)  // 构造函数，接收一个参数表示线程池中要创建的线程数量
                     // 每个线程都会在后台等待并处理任务队列的任务
    : stop(false) {  /*初始化stop为false*/
    for (size_t i = 0; i < threads; ++i)
        workers.emplace_back(  // 类内直接构造//可以传递任何类型的参数
                               // /*emplace_back()是vector里的一个函数，在数组末尾塞进一个新元素*/
                               // 捕获外界参数，以便在线程中访问成员变量
                               // /*直接创建，不是拷贝，直接在容器里构造了一个std::thread线程，让他执行lambda里的逻辑*/
            [this] { /*然后把他加到线程池的线程数组中，这个线程一创建就开始跑lambda里的代码
                      */
                     for (
                         ;
                         ;)  // 这里就是定义了一个循环的工作线程，检测任务队列是否为空，以及stop
                     {
                         std::function<void()> task;  // 任务

                         {
                             std::unique_lock<std::mutex> lock(
                                 this->queue_mutex); /*当前线程池对象自己的锁，this指向当前调用这个函数的对象本身*/
                             // 等待h唤醒，若不唤醒，则释放锁阻塞在这里并休眠
                             this->condition.wait(lock, [this] {
                                 return this->stop ||
                                        !this->tasks
                                             .empty(); /*this表示捕获当前对象，*/
                                 /*这样才能访问到stop和task（因为lambda不是类的成员，不能访问私有成员）*/
                                 /*wait根据lambda计算出的true/false判断是否继续休眠*/
                             });  // 检测，防止虚假唤醒
                             // 若stop为停止且任务队列为空，则线程退出
                             if (this->stop && this->tasks.empty())
                                 return;
                             // 接受任务
                             task = std::move(
                                 this->tasks
                                     .front()); /*移动语义，不是直接拷贝，直接把tasks队首元素的所有权限移交给task,原来位置的没了*/
                             // 弹出任务
                             this->tasks.pop();
                                 /*移动完后，队首空了，需要弹出*/ /*好处：省时间省空间*/
                         }
                         // 执行任务
                         task();
                     }
            });
}

// add new work item to the pool
template <
    class F,
    class
    ... Args>  // 定义一个变长模板，F是可调用对象，如函数lambda,functor,Args是参数
auto ThreadPool::enqueue(
    F&& f,
    Args&&... args)  // 使用尾置限定符和future获取返回值，f是可调用对象，如函数lambda,functor,args是参数
    -> std::future<typename std::result_of<F(Args...)>::type> {
    // 简化书写，推导返回值类型
    using return_type =
        typename std::result_of<F(Args...)>::type; /*typename：起别名*/
    // 使用packageed_task封装函数，设置return_type类型为返回值
    // /*std::packaged_task<return_type()>：把一个可调用对象包装成一个无参数调用对象同时绑定一个future获取返回值*/
    // 再使用make_shared创建一个shared_ptr的智能指针来方便管理对象
    // /*return_type()：包装后的函数无参数，返回return_type,统一任务格式*/
    // 最后使用bind将可执行函数F和参数args绑定到一起，生成新的可调用对象
    // /*std::forward<>：完美转发，保持参数原封不动传递，这里是传给了bind*/
    // forward完美转发，与参数的万能引用一起使用，提高效率
    // /*std::bind：接受一个可调用对象和任意数量参数，返回一个可以直接调用的打包对象，这样不用传参就能跑*/
    auto task = std::make_shared<std::packaged_task<
        return_type()> >(/*std::make_shared：智能指针，创建了一个对象，用shared_ptr管理他，可以自动释放内存，由于packaged_task只能移*/
                         std::bind(
                             std::forward<F>(f),
                             std::forward<Args>(
                                 args)...)); /*动不能拷贝但是有多处要使用所以用shared_ptr实现共享，没人用了自动销毁*/
    // 获取task的future
    std::future<return_type> res = task->get_future();
    {
        std::unique_lock<std::mutex> lock(queue_mutex);

        // don't allow enqueueing after stopping the pool
        if (stop)
            throw std::runtime_error("enqueue on stopped ThreadPool");
        // 将任务提交到任务队列中
        tasks.emplace([task]() {
            (*task)();
        }); /*emplace()也是直接构造，区别在于前面那个只适用于序列容器(?)，这个适用于所有容器*/
    } /*[task]：值捕获，task是shared_otr，拷贝可以延长任务生命周期*/
    // 唤醒工作线程
    // /*(){...}：无参lambda，它会被自动包装成std::function<void()>放进队列 */
    condition
        .notify_one(); /*(*task)()：task是shared_ptr<packaged_task<...>>,*task解引用后得到packaged_task对象*/
    return res; /*packaged_task重载了()运算符，调用它会执行真正的任务并把结果写入future*/
}

// the destructor joins all threads
inline ThreadPool::~ThreadPool() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        stop = true;  // 设置停止标志
    }
    condition.notify_all();  // 唤醒所有线程
    // 等待所有线程执行完毕并回收资源
    for (std::thread& worker : workers)
        worker.join();
}
#endif
