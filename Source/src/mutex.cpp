/*
	std::mutex implementation for C
*/

#include "mutex.h"
#include <mutex>
#include <cstdlib>
using namespace std;

extern "C" {

	void initMutex(mutex_t s)
	{
		mutex m;
		memcpy(s, &m, sizeof(m));
	}

	void destroyMutex(mutex_t s)
	{
		delete ((mutex*)s);
	}

	void wait(mutex_t s)
	{
		((mutex*)s)->lock();
	}

	void signal(mutex_t s)
	{
		((mutex*)s)->unlock();
	}

}


