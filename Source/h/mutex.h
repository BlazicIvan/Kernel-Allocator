/*
	C API for std::mutex
*/

#ifndef MUTEX_H
#define MUTEX_H

#include <stddef.h>

// Mutex type
typedef void * mutex_t;

// Size of mutex object
#ifdef _WIN64
#define MUTEX_SIZE 96
#else
#define MUTEX_SIZE 48
#endif




#ifdef __cplusplus
extern "C" {

#endif

// Initialize mutex object on allocated space
void initMutex(mutex_t sem);

// Destroy mutex object
void destroyMutex(mutex_t sem);

// Wait on sem
void wait(mutex_t sem);

// Signal on sem
void signal(mutex_t sem);



#ifdef __cplusplus
}
#endif


#endif