/***************************************************************************
                          mutex.h  -  description
                             -------------------
    begin                : Sam nov 9 2002
    copyright            : (C) 2002 by spe
    email                : spe@artik.intra.selectbourse.net
 ***************************************************************************/

#ifndef MUTEX_H
#define MUTEX_H

#include <pthread.h>
#include "log.h"
#include "debugtk.h"

// Memory debugging
// #include "debug.h"

// Externals
extern LogError *systemLog;

/**
  *@author spe
  */

class Mutex {
private:
  pthread_mutex_t mutexLock;
  pthread_mutexattr_t mutexAttributes;
  int mutexInitialized;
public:
  Mutex();
  Mutex(int);
  ~Mutex();
  int lockMutex(void);
  int unlockMutex(void);
  int tryLockMutex(void);
};

#endif
