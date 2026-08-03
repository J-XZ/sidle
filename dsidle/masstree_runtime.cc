// D-SIDLE only links the Masstree logging translation unit so that the
// existing Masstree object layout remains intact.  Recovery/log replay is not
// a D-SIDLE execution path, but log.cc has a small set of upstream process
// globals which the archived benchmark normally supplies.  Keep those
// bindings local to this target instead of silently dropping log.cc or
// deleting the legacy source.
#include <pthread.h>

#include "third_party/masstree-beta/log.hh"
#include "third_party/masstree-beta/query_masstree.hh"

Masstree::default_table* tree = nullptr;
volatile bool recovering = false;
pthread_mutex_t rec_mu = PTHREAD_MUTEX_INITIALIZER;

void waituntilphase(int) {}
void inactive() {}
