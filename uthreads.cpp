#include "uthreads.h"
#include <iostream>
#include <deque>
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>
#include <sys/time.h>

#define SECOND 1000000
#ifdef __x86_64__
/* code for 64 bit Intel arch */

typedef unsigned long address_t;
#define JB_SP 6
#define JB_PC 7

/* A translation is required when using an address of a variable.
   Use this as a black box in your code. */
address_t translate_address(address_t addr)
{
    address_t ret;
    asm volatile("xor    %%fs:0x30,%0\n"
		"rol    $0x11,%0\n"
                 : "=g" (ret)
                 : "0" (addr));
    return ret;
}

#else
/* code for 32 bit Intel arch */

typedef unsigned int address_t;
#define JB_SP 4
#define JB_PC 5

/* A translation is required when using an address of a variable.
   Use this as a black box in your code. */
address_t translate_address(address_t addr)
{
    address_t ret;
    asm volatile("xor    %%gs:0x18,%0\n"
                 "rol    $0x9,%0\n"
    : "=g" (ret)
    : "0" (addr));
    return ret;
}

#endif

enum ThState { RUNNING, BLOCKED, READY };


class Thread {
private:
    char* stack;

public:
    int tid;
    ThState state;
    void (*f)();
    sigjmp_buf env;
    int quantums_counter;
    int quantum;


    Thread(int tid, int quantum, void (*f)()):
            stack(nullptr),
            tid(tid),
            state(READY),
            f(f),
            env(),
            quantums_counter(0),
            quantum(quantum)
    {
        try {
            stack = new char[STACK_SIZE];
        } catch (const std::bad_alloc& e) {
            std::cerr << "system error: " << e.what() << std::endl;
            exit(1);
        }

        if (tid == 0) {

            address_t sp;
            sp = (address_t)(stack) + STACK_SIZE - sizeof(address_t);
            sigsetjmp(env, 1);
            (env->__jmpbuf)[JB_SP] = translate_address(sp);

            state = RUNNING;
        } else {

            address_t sp, pc;
            sp = (address_t)(stack) + STACK_SIZE - sizeof(address_t);
            pc = (address_t)f;
            sigsetjmp(env, 1);
            (env->__jmpbuf)[JB_SP] = translate_address(sp);
            (env->__jmpbuf)[JB_PC] = translate_address(pc);
        }


        if (sigemptyset(&env->__saved_mask) < 0) {
            std::cerr << "system error: sigemptyset failed" << std::endl;
            exit(1);
        }

        // design decision: each thread holds a default mask of SIGVTALRM
        // (by default the mask doesn't applied to sigprocmask)
        if (sigaddset(&env->__saved_mask, SIGVTALRM)){
            std::cerr << "system error: sigaddset failed" << std::endl;
            exit(1);
        }
    }

    ~Thread(){
        delete[] stack;
    }
};


class ThreadsPool{
private:
    int *quantums;
    int n_quantums;
    struct itimerval timer;

    std::deque<Thread*> ready_threads;
    Thread* threads[MAX_THREAD_NUM];

    int current_tid;
    int quantums_counter;

    int _get_free_id(){
        for (int i = 0; i < MAX_THREAD_NUM; i++) {
            if (threads[i] == nullptr) {
                return i;
            }
        }
        return -1;
    }

    void _remove_from_ready_q(int tid){
        for (auto it = ready_threads.begin(); it != ready_threads.end() ;) {
            if ((*it)->tid == tid) {
                it = ready_threads.erase(it);
            } else {
                ++it;
            }
        }
    }

    void _handle_sigvtalrm_during_self_termination_or_self_block(){
        sigset_t waiting_mask;
        if (sigpending (&waiting_mask) < 0){
            std::cerr << "system error: sigpending failed" << std::endl;
            exit(1);
        }

        int is_a_member = sigismember (&waiting_mask, SIGVTALRM);
        if (is_a_member < 0){
            std::cerr << "system error: sigismember failed" << std::endl;
            exit(1);
        } else if (is_a_member) {

            struct sigaction ign_signtalrm, prev;
            ign_signtalrm.sa_handler = SIG_IGN;
            ign_signtalrm.sa_flags = 0;
            sigemptyset(&ign_signtalrm.sa_mask);

            // ignore the pending SIGVTALRM
            if (sigaction(SIGVTALRM, &ign_signtalrm, &prev) < 0) {
                std::cerr << "system error: sigaction failed" << std::endl;
                exit(1);
            }

            if (sigaction(SIGVTALRM, &prev, nullptr) < 0) {
                std::cerr << "system error: sigaction failed" << std::endl;
                exit(1);
            }
        }
    }


public:
    ThreadsPool(const int *quantum_usecs, int size, int* err_code):
            quantums(nullptr),
            n_quantums(size),
            timer(),
            ready_threads(),
            threads(),
            current_tid(0),
            quantums_counter(0)
    {
        if (size <= 0) {
            std::cerr << "thread library error: invalid quantum_usecs arr size" << std::endl;
            *err_code = -1;
        }
        try {
            quantums = new int[size];
        } catch (const std::bad_alloc& e) {
            std::cerr << "system error: " << e.what() << std::endl;
            exit(1);
        }

        for (int i = 0; i < size; i++) {
            if (quantum_usecs[i] <= 0){
                std::cerr << "thread library error: quantum_usecs must be positive" << std::endl;
                *err_code = -1;
            }
            quantums[i] = quantum_usecs[i];
        }
    }

    ~ThreadsPool(){
        delete [] quantums;
        for (auto t: threads){
            delete t;
        }
    }

    void switch_th(bool self_term = false)
    {
        if (!self_term) {
            int ret_val = sigsetjmp(threads[current_tid]->env,1);
            if (ret_val == 1) {
                return;
            }

            if (threads[current_tid]->state == RUNNING){
                threads[current_tid]->state = READY;
                ready_threads.push_back(threads[current_tid]);
            }
        }

        if (!ready_threads.empty()){
            Thread* next_th = ready_threads.front();
            ready_threads.pop_front();
            current_tid = next_th->tid;

            // unblock thread relevant signals, in case that previous thread blocked it
            if (sigprocmask(SIG_UNBLOCK, &threads[current_tid]->env->__saved_mask, nullptr) < 0){
                std::cerr << "system error: sigprocmask failed" << std::endl;
                exit(1);
            }

            quantums_counter++;
            threads[current_tid]->quantums_counter++;
            threads[current_tid]->state = RUNNING;

            timer.it_value = {0, threads[current_tid]->quantum};
            timer.it_interval = {0, 0};

            if (setitimer (ITIMER_VIRTUAL, &timer, nullptr) < 0) {
                std::cerr << "system error: setitimer failed" << std::endl;
                exit(1);
            }

            siglongjmp(next_th->env,1);
        }
    }

    int assign(void (*f)(), int priority) {
        int tid = _get_free_id();
        if (tid == -1) {
            std::cerr << "thread library error: max num of threads exceeded" << std::endl;
            return -1;
        }

        if (priority < 0 || priority >= n_quantums) {
            std::cerr << "thread library error: invalid priority" << std::endl;
            return -1;
        }

        threads[tid] = new Thread(tid, quantums[priority], f);

        if (tid != 0) {
            ready_threads.push_back(threads[tid]);
        }
        return tid;
    }

    int change_priority(int tid, int priority){
        if (tid < 0 || tid >= MAX_THREAD_NUM || threads[tid] == nullptr){
            std::cerr << "thread library error: invalid thread id" << std::endl;
            return -1;
        }
        threads[tid]->quantum = quantums[priority];
        return 0;
    }

    int terminate(int tid) {
        if (sigprocmask(SIG_SETMASK, &(threads[current_tid]->env->__saved_mask), nullptr) < 0){
            std::cerr << "system error: sigprocmask failed" << std::endl;
            exit(1);
        }

        if (tid < 0 || tid >= MAX_THREAD_NUM || threads[tid] == nullptr){
            std::cerr << "thread library error: invalid thread id" << std::endl;
            return -1;
        }

        if (current_tid == tid){  // thread terminates itself

            _remove_from_ready_q(tid);
            delete threads[tid];
            threads[tid] = nullptr;
            _handle_sigvtalrm_during_self_termination_or_self_block();
            switch_th(true);

        }else {

            _remove_from_ready_q(tid);
            delete threads[tid];
            threads[tid] = nullptr;

            if (sigprocmask(SIG_UNBLOCK, &(threads[current_tid]->env->__saved_mask), nullptr) < 0){
                std::cerr << "system error: sigprocmask failed" << std::endl;
                exit(1);
            }
        }
        return 0;
    }

    int block(int tid) {
        if (sigprocmask(SIG_SETMASK, &(threads[current_tid]->env->__saved_mask), nullptr) < 0){
            std::cerr << "system error: sigprocmask failed" << std::endl;
            exit(1);
        }

        if (tid <= 0 || tid >= MAX_THREAD_NUM || threads[tid] == nullptr){
            std::cerr << "thread library error: invalid thread id" << std::endl;
            return -1;
        }

        if (current_tid == tid){  // thread blocks itself
            threads[tid]->state = BLOCKED;
            _remove_from_ready_q(tid);
            _handle_sigvtalrm_during_self_termination_or_self_block();
            switch_th();
            return 0;

        } else if (threads[tid]->state != BLOCKED){
            threads[tid]->state = BLOCKED;
            _remove_from_ready_q(tid);

            if (sigprocmask(SIG_UNBLOCK, &(threads[current_tid]->env->__saved_mask), nullptr) < 0){
                std::cerr << "system error: sigprocmask failed" << std::endl;
                exit(1);
            }
        }
        return 0;
    }

    int resume(int tid) {
        if (tid < 0 || tid >= MAX_THREAD_NUM || threads[tid] == nullptr){
            std::cerr << "thread library error: invalid thread id" << std::endl;
            return -1;
        }
        if (threads[tid]->state == BLOCKED){
            threads[tid]->state = READY;
            ready_threads.push_back(threads[tid]);
        }
        return 0;
    }

    int get_current_tid(){
        return current_tid;
    }

    int get_total_quantums(){
        return quantums_counter;
    }

    int get_th_quantums(int tid){
        if (tid < 0 || tid >= MAX_THREAD_NUM || threads[tid] == nullptr){
            std::cerr << "thread library error: invalid thread id" << std::endl;
            return -1;
        }
        return threads[tid]->quantums_counter;
    }
};


ThreadsPool* threads;


struct sigaction sa = {nullptr};


void timer_handler(int sig)
{
    if (sig == SIGVTALRM){
        threads->switch_th();
    }
}


int uthread_init(int *quantum_usecs, int size) {
    sa.sa_handler = &timer_handler;

    if (sigaction(SIGVTALRM, &sa, nullptr) < 0) {
        std::cerr << "system error: sigaction failed" << std::endl;
        exit(1);
    }

    int err_code = 0;
    threads = new ThreadsPool(quantum_usecs, size, &err_code);
    if (err_code < 0){
        return err_code;
    }
    int res = threads->assign(nullptr, 0);
    threads->switch_th();
    return res;
}

int uthread_spawn(void (*f)(), int priority){
    return threads->assign(f, priority);
}

int uthread_change_priority(int tid, int priority){
    return threads->change_priority(tid, priority);
}

int uthread_terminate(int tid){
    if (tid == 0) {
        delete threads;
        exit(0);
    }
    return threads->terminate(tid);
}

int uthread_block(int tid) {
    return threads->block(tid);
}

int uthread_resume(int tid){
    return threads->resume(tid);
}

int uthread_get_tid(){
    return threads->get_current_tid();
}

int uthread_get_total_quantums(){
    return threads->get_total_quantums();
}

int uthread_get_quantums(int tid){
    return threads->get_th_quantums(tid);
}
