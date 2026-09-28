// .cmi images: the decoded graph of a .cmi, cached on disk and mapped back
// instead of decoded (not an OCaml module; c++ocamlc's own).
//
// read_cmi (cmi_format.cpp) decodes a .cmi into zone objects -- a pure
// function of its bytes: the objects point only at one another and at the
// few static singletons of types.cpp (tnil, cok, ...), and decoding
// advances no counter.  So a snapshot of those objects is as good as a
// fresh decode.  The first decode of a .cmi allocates its objects in a
// region mapped at a fixed address chosen from the file's identity, and
// writes the region out; later compilations map the file back at that same
// address (MAP_PRIVATE: the typer's mutations of loaded types stay
// private), with no pointer to fix up.  The singletons live in a small
// region pinned at a fixed address for the same reason (the executable is
// position-independent).
//
// An image is keyed by the .cmi's file identity (device, inode, size,
// mtime, ctime) and by c++ocamlc's build (its GNU build-id), and is only
// used when every check passes; anything else -- a missing, stale,
// truncated or foreign image, a damaged header, an address already taken --
// falls back to decoding the .cmi.  Images are written to a temporary file
// and renamed (concurrent compilations race harmlessly).  What makes an
// image valid is that the Reader's objects reference nothing but one
// another and the pinned singletons: its constructors copy strings into the
// zone and take no other storage, counter or table (keep it that way).
//
// Environment: CPPCAML_CMI_CACHE=0 disables the cache, =<dir> puts it in
// <dir> (default $XDG_CACHE_HOME/c++ocamlc/cmi, else
// ~/.cache/c++ocamlc/cmi); CPPCAML_CMI_CACHE_VERIFY=1 also checks each
// image's data checksum when it is mapped (~5% of a warm compilation: it
// touches every page); CPPCAML_CMI_CACHE_MAX=<MB> caps the cache's size
// (default 2048; the oldest images go first); CPPCAML_CMI_CACHE_DEBUG=1
// says why an image is not used or not recorded, =2 also counts the words
// of a recorded image that look like pointers elsewhere (uninitialized
// padding of the Reader's temporaries does).
#pragma once

#include <cstddef>
#include <new>
#include <optional>
#include <string>
#include <utility>

#include "cppcaml/typing/cmi_format.hpp"

namespace cppcaml::typing::cmi_image {

// Storage in the pinned region (fixed address), or the heap when it could
// not be mapped (the cache is then disabled).  For process-long singletons
// that .cmi images point at.
void* pinned_alloc(std::size_t n, std::size_t align);
template <class T, class... A>
T* pinned_new(A&&... a) {
  return new (pinned_alloc(sizeof(T), alignof(T))) T{std::forward<A>(a)...};
}

// The image of [filename], mapped: its CmiInfos, or nullopt (decode it).
std::optional<cmi_format::CmiInfos> load(const std::string& filename);

// Recording the first decode of a .cmi: `decode` runs with the current zone
// allocating in an image region; its result is written out as the image.
// Returns nullopt when no image can be recorded for it (the caller decodes
// as usual).  Exceptions from decode propagate (nothing is recorded).
template <class F>
std::optional<cmi_format::CmiInfos> record(const std::string& filename, std::size_t file_size, F&& decode);

// ---- internals of record ----
struct Recorder;
Recorder* begin_record(const std::string& filename, std::size_t file_size);
void enter(Recorder* r);                                  // its zone becomes current
void leave(Recorder* r);                                  // the previous zone again
void commit(Recorder* r, const cmi_format::CmiInfos& ci);  // write the image
void abandon(Recorder* r);                                // decode failed / region full

template <class F>
std::optional<cmi_format::CmiInfos> record(const std::string& filename, std::size_t file_size, F&& decode) {
  Recorder* r = begin_record(filename, file_size);
  if (!r) return std::nullopt;
  cmi_format::CmiInfos ci;
  try {
    enter(r);
    ci = decode();
    leave(r);
  } catch (const Zone::RegionFull&) {
    leave(r);
    abandon(r);
    return std::nullopt;
  } catch (...) {
    leave(r);
    abandon(r);
    throw;
  }
  commit(r, ci);
  return ci;
}

}  // namespace cppcaml::typing::cmi_image
