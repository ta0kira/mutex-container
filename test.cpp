#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>

#include "mutex-container.hpp"

//(probably better as arguments, but I'm too lazy right now)
#define THREADS 10
#define TIME    30
//(if you set either of these to 'false', the threads will gradually die off)
#define READ_BLOCK  true
#define WRITE_BLOCK true


//the data being protected
typedef mutex_container <int> protected_int;
static protected_int my_data;

//protect the output file while we're at it
typedef mutex_container <FILE*, w_lock> protected_out;
static protected_out stdout2(stdout);

static void send_output(protected_out &out, const char *format, ...);

static void *thread(void *nv);


int main()
{
  //make sure assignment works properly
  mutex_container <long, broken_lock <true> > my_data2(THREADS);
  //NOTE: this attempts to lock the mutex, and causes an assertion on failure!
  my_data = my_data2;

  //create some threads
  pthread_t threads[THREADS];
  for (long i = 0; i < sizeof threads / sizeof(pthread_t); i++) {
    send_output(stdout2, "start %li\n", i);
    threads[i] = pthread_t();
    if (pthread_create(threads + i, NULL, &thread, (void*) i) != 0) {
      send_output(stdout2, "error: %s\n", strerror(errno));
    }
  }

  //wait for them to do some stuff
  sleep(TIME);

  //the threads exit when the value goes below 0
  //NOTE: this attempts to lock the mutex, and causes an assertion on failure!
  my_data = -1;

  for (long i = 0; i < sizeof threads / sizeof(pthread_t); i++) {
    pthread_join(threads[i], NULL);
    send_output(stdout2, "join %li\n", i);
  }
}


//a print function that ensures we have exclusive access to the output
static void send_output(protected_out &out, const char *format, ...) {
  va_list ap;
  va_start(ap, format);
  protected_out::proxy write = out.get();
  if (!write) return;
  vfprintf(*write, format, ap);
}


//a simple thread for repeatedly accessing the data
static void *thread(void *nv) {
  //(cancelation can be messy...)
  if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL) != 0) return NULL;

  long n = (long) nv;
  struct timespec wait = { 0, (10 + n) * 10 * 1000 * 1000 };
  nanosleep(&wait, NULL);

  //loop through reading and writing forever

  while (true) {
    //read a bunch of times

    for (int i = 0; i < THREADS + n; i++) {
      send_output(stdout2, "?read %li\n", n);
      protected_int::const_proxy read = my_data.get_const(READ_BLOCK);
      if (!read) {
        send_output(stdout2, "!read %li\n", n);
        return NULL;
      }

      send_output(stdout2, "+read %li (%i) -> %i\n", n, read.last_lock_count(), *read);
      if (*read < 0) return NULL;
      nanosleep(&wait, NULL);

      read.clear();
      send_output(stdout2, "-read %li\n", n);
      nanosleep(&wait, NULL);
    }

    //write once

    send_output(stdout2, "?write %li\n", n);
    protected_int::proxy write = my_data.get(WRITE_BLOCK);
    if (!write) {
      send_output(stdout2, "!write %li\n", n);
      return NULL;
    }

    send_output(stdout2, "+write %li (%i)\n", n, write.last_lock_count());
    if (*write < 0) return NULL;
    *write = n;
    nanosleep(&wait, NULL);

    write.clear();
    send_output(stdout2, "-write %li\n", n);
    nanosleep(&wait, NULL);
  }
}