#include <onion/credentials_guard.h>

#include <errno.h>
#include <pthread.h>

static pthread_once_t credentials_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t credentials_mutex;
static int credentials_init_error = EAGAIN;

static void credentials_initialize(void) {
  pthread_mutexattr_t attributes;
  int error = pthread_mutexattr_init(&attributes);
  if (error) {
    credentials_init_error = error;
    return;
  }

  error = pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
  if (error) {
    (void)pthread_mutexattr_destroy(&attributes);
    credentials_init_error = error;
    return;
  }

  error = pthread_mutex_init(&credentials_mutex, &attributes);
  int destroy_error = pthread_mutexattr_destroy(&attributes);
  if (error) {
    credentials_init_error = error;
    return;
  }
  if (destroy_error) {
    /* Initialization did not finish cleanly; never expose this mutex. */
    (void)pthread_mutex_destroy(&credentials_mutex);
    credentials_init_error = destroy_error;
    return;
  }
  credentials_init_error = 0;
}

static int credentials_ready(void) {
  int error = pthread_once(&credentials_once, credentials_initialize);
  return error ? error : credentials_init_error;
}

int hhp_self_credentials_lock(void) {
  int error = credentials_ready();
  return error ? error : pthread_mutex_lock(&credentials_mutex);
}

int hhp_self_credentials_unlock(void) {
  int error = credentials_ready();
  return error ? error : pthread_mutex_unlock(&credentials_mutex);
}
