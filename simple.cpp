/* This is a very simple example of the semantics of 'mutex_container'. For a
 * non-trivial example, see 'test.cpp'.
 *
 * Compile this program enabling C++11, and linking with libpthread if needed.
 * When you run this program, you should see no output or errors. An assertion
 * means a bug in the code.
 */

#include <assert.h>

#include "mutex-container.hpp"


int main() {
  //default: use 'rw_lock'
  typedef mutex_container <int> protected_int0;

  //use a 'w_lock' instead
  typedef mutex_container <int, w_lock> protected_int1;

  //the two types above share the same base class because the both protect 'int'
  typedef protected_int0::base base;

  //protected data
  protected_int0 data0;
  protected_int1 data1;

  //authorization object to prevent deadlocks (one per thread)
  //NOTE: this will correspond to 'w_lock', since that's what 'protected_int1' uses
  protected_int1::auth_type auth(protected_int1::new_auth());
  //make sure an authorization was provided
  assert(auth);

  //proxy objects for accessing the protected data (use them like pointers)
  base::proxy       write;
  base::const_proxy read;

  //get a proxy, without deadlock prevention
  write = data0.get();
  assert(write); //(just for testing)
  //release the lock
  write.clear();

  //get a proxy, with deadlock prevention
  write = data0.get_auth(auth);
  assert(write);
  //NOTE: this updates 'auth', since 'get_auth' was used!
  write.clear();

  //get a read-only proxy
  read = data0.get_auth_const(auth);
  assert(read);
  read.clear();

  //you can use the same proxy object with different lock types
  read = data1.get_auth_const(auth);
  assert(read);

  {
    //this should fail, since 'auth' still holds a read lock
    base::proxy write2 = data0.get_auth(auth);
    //(make sure the reason for failure is what we expected)
    if (!write2) assert(!auth->lock_allowed(false));
  } //<-- 'write2' goes out of scope, which would unlock 'data0' of a lock was obtained

  {
    //copy the proxy object
    base::const_proxy read2 = read;
    assert(read2);
  } //<-- 'read2' goes out of scope, but 'data1' doesn't get unlocked since it's not a new lock

  assert(read);
  read.clear();
}