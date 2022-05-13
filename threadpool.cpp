//
// Created by cqupt1811 on 2022/3/12.
//
#include<vector>
#include<queue>
#include<atomic>
#include<future>
// 下面三个文件在futrue中已经包含
#include<thread>
#include<functional>
#include<stdexcept>
#include<iostream>
#include<string>

using namespace std;

#define THREADPOOL_MAX_NUM 16
//#define THREADPOOL_AUTO_GROW

/**
 * 线程池类，可以提交可变参数函数或者lambda表达式匿名函数执行，同时支持返回一个future对象
 */
class threadpool {
private:
    using Task = function<void()>;  // 定义任务队列中任务的类型
    vector<thread> m_pool;  // 保存线程的向量
    queue<Task> m_task;  // 任务队列
    mutex m_lock;  // 互斥量
    condition_variable m_task_cv;  // 条件变量，使用它来实现等待唤醒逻辑
    atomic<bool> m_run{true};  // 原子量，初始化为true，用以指示线程池状态，这里用括号初始化会被误认为是函数，所以用{}初始化
    atomic<int> m_idl_thread_num{0};  // 原子量，初始化为0，用以指示空闲线程的数量

public:
    /**
     * 构造函数，默认线程数量为4
     */
    explicit threadpool(unsigned short size = 4) {
        addThread(size);
    }

    /**
     * 析构函数，析构时需要将所m_run置为空，并将存在的线程执行完
     */
    ~threadpool() {
        m_run = false;
        m_task_cv.notify_all();  // 唤醒所有线程
        for (auto &thd: m_pool) {
            if (thd.joinable()) {
                thd.join();  // 等待所有线程执行完毕
            }
        }
    }

    /**
     * 返回空闲线程的数量
     */
    int idlThreadCount() const {
        return m_idl_thread_num;
    }

    /*
     * 返回线程池的大小
     */
    int threadCount() const {
        return m_pool.size();
    }

    /**
     * 提交一个任务，即各种函数类型调用的对象，可以是函数指针、function对象、bind获得的对象
     * 该方法使用尾置返回类型（因为这种模板的写法就是只能使用尾置返回类型推断），返回类型为future，接收方获取到future变量后可以调用get()方法获取函数返回的值
     * 可以使用下面两种方法调用类成员函数
     * 一、使用bind: .commit(std::bind(&Dog::bark, &dog))
     * 二、使用mem_fn: .commit(std::men_fn(&Dog::bark), this);
     * 使用可变模板实现对不同参数的函数的支持
     */
    template<typename F, typename... Args>
    auto commit(F &&f, Args &&... args) -> future<decltype(f(args...))> {
        // 如果状态是暂停，那么直接抛出错误
        if (!m_run) {
            throw runtime_error("thread pool is stoped.");
        }

        // 使用decltype获取返回类型
        using RetType = decltype(f(args...));
        // 使用share_ptr，该指针指向一个由bind绑定的functional对象构造的packaged_task对象
        // 由于bind直接将所有参数绑定到了传入的函数f上，所以执行(*task)()时无需再传入任何参数
        auto task = make_shared<packaged_task<RetType()>>(bind(forward<F>(f), forward<Args>(args)...));
        // 获取返回的future对象
        future<RetType> res = task->get_future();

        unique_lock<mutex> u_lock(m_lock);
        // 使用lambda表达式获取一个函数对象，在该函数内调用传入的任务函数
        auto lbd = [task]() {
            (*task)(); // 解引用指针调用函数
        };
        // 使用lambda表达式所得函数在任务队列尾部生成一个task对象
        m_task.emplace(lbd);

        // 因为插入了一个任务，所以唤醒一个线程
        m_task_cv.notify_one();
        // 返回一i个future对象，接受可以获取函数执行后的值
        return res;
    }


private:
    /**
     * 将指定数量的线程添加到线程向量中
     */
    void addThread(unsigned short size) {
        // 向线程向量中添加线程，但是不超过规定的最大值
        for (; m_pool.size() < THREADPOOL_MAX_NUM && size > 0; --size) {
            // 使用lambda表达式在直接向量中创建thread对象，传进this指针，所以可以使用类中变量
            auto lbd = [this]() {
                //  如果m_run为true，那么这个线程就是一直运行，没有任务时会卡在wait()位置，等待notify唤醒，唤醒拿到任务后执行拿到的任务，如此循环
                while (m_run) { // 如果将这里改为true，那么线程会执行完队列中的所有任务之后才销毁

                    unique_lock<mutex> u_lock(m_lock);  // 创建一个unique_lock对象，为了传入condition variable中
                    m_task_cv.wait(u_lock, [this]() {
                        // 如果任务队列为空则堵塞，非空则进入下一步，从队列中取出一个任务，同时，若m_run为false也取消阻塞，这说明将要销毁线程了
                        return !m_run || !m_task.empty();
                    });  // 第二个参数一个lambda表达式，表示阻塞的条件，该表达式返回false会阻塞线程并释放锁，true不阻塞

                    // 如果m_run为false且任务队列为空，那可以直接返回
                    if (!m_run && m_task.empty()) {
                        return;
                    }
                    // 从任务队列中取出一个任务
                    Task task = std::move(m_task.front());  // 获取一个任务
                    m_task.pop();
                    printf("------------------------------------------------%d\n",
                           m_idl_thread_num.load());  // 打印一下当前空闲的线程
                    u_lock.unlock();  // 提前解锁，否则u_lock对象析构时才解锁

                    m_idl_thread_num--;  // 任务开始执行，空闲线程减一
                    task(); // 执行任务
                    m_idl_thread_num++;  // 任务执行完成，空闲线程加一
                }
            };
            // 在向量的尾部以lbd为参数创建thread对象
            m_pool.emplace_back(lbd);

            m_idl_thread_num++; // 一个线程创建完成，空闲线程加一
        }
    }
};


/**
 * 返回值为void，形参为空的函数
 */
void func1() {
    this_thread::sleep_for(chrono::milliseconds(200));
    printf("this is func1\n");
}

/**
 * 返回值为void，只有一个int形参的函数
 */
void func2(int a) {
    this_thread::sleep_for(chrono::milliseconds(200));
    printf("parameter 1 is: %d\n", a);
}

/**
 * 返回值为void，有两个int形参的函数
 */
void func3(int a, int b) {
    this_thread::sleep_for(chrono::milliseconds(200));
    printf("parameter 1 is %d, and parameter 2 is %d\n", a, b);
}

/**
 * 返回值为void，有两个不同类型形参的函数
 */
void func4(int a, string b) {
    this_thread::sleep_for(chrono::milliseconds(200));
    printf("parameter 1 is %d, and parameter 2 is %s\n", a, b.c_str());
}

/**
 * 返回值为int，只有一个形参的函数
 */
int func5(int a) {
    this_thread::sleep_for(chrono::milliseconds(200));
    return a * a;
}


/**
 * 返回值为int，只有一个形参的函数
 */
string func6(int a, string b) {
    this_thread::sleep_for(chrono::milliseconds(200));
    return b + to_string(a);
}

/**
 * 一个普通的类
 */
class A {
public:
    /**
     * 静态函数
     */
    static string func7(int a, string b) {
        this_thread::sleep_for(chrono::milliseconds(200));
        return to_string(a) + b;
    }

    /**
     * 普通成员函数
     */
    string func8(int a, string b) {
        this_thread::sleep_for(chrono::milliseconds(200));
        return to_string(a) + b;
    }
};

/**
 * 仿函数
 */
class B {
public:
    string operator()(int a, string b) {
        this_thread::sleep_for(chrono::milliseconds(200));
        return to_string(a) + b;
    }
};

int main() {
    threadpool my_threadpool(3);  // 初始化五条线程的线程池

    my_threadpool.commit(func1);  // 调用无参，无返回值的函数

    my_threadpool.commit(func2, 66);  // 调用一个参数，无返回值的函数

    my_threadpool.commit(func3, 77, 88);  // 调用两个相同形参，无返回值的函数

    my_threadpool.commit(func4, 99, "func4 test");  // 调用两个相同形参，无返回值的函数


    // =======================================================================
    auto fut1 = my_threadpool.commit(func5, 9);  // 调用一个参数，并有返回值的函数
    printf("return value is: %d\n", fut1.get());  // 从返回的future对象中拿到值

    auto fut2 = my_threadpool.commit(func6, 12345, "54321");  // 调用两个不同参数，并有返回值的函数
    printf("return value is: %s\n", fut2.get().c_str());  // 从返回的future对象中拿到值

    // =======================================================================
    auto fut3 = my_threadpool.commit([](int a, int b) -> string {  // 使用lambda表达式
        this_thread::sleep_for(chrono::milliseconds(20));
        return to_string(a) + to_string(b) + "-lambda";
    }, 648, 128);
    printf("return value is: %s\n", fut3.get().c_str());  // 从返回的future对象中拿到值

    auto fut4 = my_threadpool.commit(B(), 847, " function like object");  // 使用仿函数
    printf("return value is: %s\n", fut4.get().c_str());  // 从返回的future对象中拿到值


    // =======================================================================
    A a;

    auto fut5 = my_threadpool.commit(&A::func7, 10000, "zzh");  // 使用类的静态函数
    printf("return value is: %s\n", fut5.get().c_str());  // 从返回的future对象中拿到值

    // 无法直接调用非静态
//    auto fut6 = my_threadpool.commit(&A::func8, &a, 8765, "dsdsf");  // 调用非静态函数
//    printf("return value is: %s\n", fut5.get().c_str());  // 从返回的future对象中拿到值


    // =======================================================================
    // 使用bind包装一下然后调用类中非静态函数
    auto fut7 = my_threadpool.commit(bind(&A::func8, &a, placeholders::_1, placeholders::_2), 666, " using bind");
    printf("return value is: %s\n", fut7.get().c_str());  // 从返回的future对象中拿到值


    this_thread::sleep_for(chrono::milliseconds(2000));

}
