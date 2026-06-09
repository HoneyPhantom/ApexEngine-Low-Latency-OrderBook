#pragma once
#include <thread>

#if defined(__APPLE__)
    #include <pthread.h>
    #include <mach/mach.h>
    #include <mach/thread_policy.h>
#elif defined(__linux__)
    #include <pthread.h>
    #include <sched.h>
#endif

inline void pin_thread_to_core(int core_id) {
#if defined(__APPLE__)
    thread_affinity_policy_data_t policy = { core_id };
    thread_port_t mach_thread = pthread_mach_thread_np(pthread_self());
    thread_policy_set(mach_thread, THREAD_AFFINITY_POLICY, (thread_policy_t)&policy, THREAD_AFFINITY_POLICY_COUNT);
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#elif defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#endif
}