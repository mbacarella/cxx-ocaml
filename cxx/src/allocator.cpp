// The allocator's settings, where mimalloc is linked in (Makefile: the
// vendored static override object; MIMALLOC=no builds without it).
#ifdef CPPCAML_HAVE_MIMALLOC
#include <mimalloc.h>
#endif

#include <cstdlib>

namespace cppcaml {

// Freed memory back to the system at once (mimalloc's default waits a
// second): the .cmx readers' and the passes' scratch is allocated and freed
// again and again within a compilation, and with the delay it all stays
// resident -- a third of a large unit's peak.  MIMALLOC_PURGE_DELAY still
// overrides.
void allocator_purge_immediately() {
#ifdef CPPCAML_HAVE_MIMALLOC
  if (!std::getenv("MIMALLOC_PURGE_DELAY")) mi_option_set(mi_option_purge_delay, 0);
#endif
}

}  // namespace cppcaml
