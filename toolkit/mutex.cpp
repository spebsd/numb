/***************************************************************************
                          mutex.cpp  -  description
                             -------------------
    begin                : Sam nov 9 2002
    copyright            : (C) 2002 by spe
    email                : spe@artik.intra.selectbourse.net
 ***************************************************************************/

#include <errno.h>
#include "mutex.h"

Mutex::Mutex() {
  int returnCode;

  mutexInitialized = 0;
  returnCode = pthread_mutexattr_init(&mutexAttributes);
  if (returnCode == EINVAL) {
    systemLog->sysLog(ERROR, "mutexAttributes argument is not valid, cannot initialize mutex\n");
    return;
  }
  returnCode = pthread_mutex_init(&mutexLock, &mutexAttributes);
  if (returnCode) {
    switch (returnCode) {
      case EINVAL:
        systemLog->sysLog(ERROR, "mutexLock argument is not valid, cannot initialize mutex\n");
        break;
      case ENOMEM:
        systemLog->sysLog(ERROR, "insufficient memory exists to initialize to initialize the mutex\n");
        break;
      case EAGAIN:
        systemLog->sysLog(ERROR, "the temporarily lacks the resources to create another mutex\n");
        break;
      }
  }
  else
    mutexInitialized = 1;

  return;
}

Mutex::Mutex(int mutexType) {
  int returnCode;

  mutexInitialized = 0;
  returnCode = pthread_mutexattr_init(&mutexAttributes);
  if (returnCode == EINVAL) {
    systemLog->sysLog(ERROR, "mutexAttributes argument is not valid, cannot initialize mutex\n");
    return;
  }
  returnCode = pthread_mutexattr_settype(&mutexAttributes, mutexType);
  if (returnCode == EINVAL) {
    systemLog->sysLog(ERROR, "mutexAttributes/mutexType argument(s) is/are not valid, cannot set type of mutex\n");
    return;
  }
  returnCode = pthread_mutex_init(&mutexLock, &mutexAttributes);
  if (returnCode) {
    switch (returnCode) {
      case EINVAL:
        systemLog->sysLog(ERROR, "mutexLock argument is not valid, cannot initialize mutex\n");
        break;
      case ENOMEM:
        systemLog->sysLog(ERROR, "insufficient memory exists to initialize to initialize the mutex\n");
        break;
      case EAGAIN:
        systemLog->sysLog(ERROR, "the temporarily lacks the resources to create another mutex\n");
        break;
      }
  }
  else
    mutexInitialized = 1;

  return;
}

Mutex::~Mutex() {
  if (mutexInitialized) {
    pthread_mutex_destroy(&mutexLock);
    pthread_mutexattr_destroy(&mutexAttributes);
  }
  mutexInitialized = 0;

  return;
}

int Mutex::lockMutex(void) {
  int returnCode;

  if (! mutexInitialized) {
    systemLog->sysLog(ERROR, "the mutexLock is not initialized correctly, cannot use it\n");
    return EINVAL;
  }
  returnCode = pthread_mutex_lock(&mutexLock);
  if (returnCode == EINVAL)
    systemLog->sysLog(ERROR, "the mutexLock argument is invalid, cannot lock the mutex\n");
  if (returnCode == EDEADLK)
    systemLog->sysLog(ERROR, "a deadlock problem occured while trying to lock the mutex\n");

  return returnCode;
}

int Mutex::unlockMutex(void) {
  int returnCode;

  if (! mutexInitialized) {
    systemLog->sysLog(ERROR, "the mutexLock is not initialized correctly, cannot use it\n");
    return EINVAL;
  }
  returnCode = pthread_mutex_unlock(&mutexLock);
  if (returnCode == EINVAL)
    systemLog->sysLog(ERROR, "the mutexLock argument is invalid, cannot unlock the mutex\n");
  if (returnCode == EPERM)
    systemLog->sysLog(ERROR, "the current thread does not hold a lock on mutex\n");

  return returnCode;
}

int Mutex::tryLockMutex(void) {
  int returnCode;

  if (! mutexInitialized) {
    systemLog->sysLog(ERROR, "the mutexLock is not initialized correctly, cannot use it\n");
    return EINVAL;
  }
  returnCode = pthread_mutex_trylock(&mutexLock);
  if (returnCode == EINVAL)
    systemLog->sysLog(ERROR, "the mutexLock argument is invalid, cannot lock the mutex\n");

  return returnCode;
}
