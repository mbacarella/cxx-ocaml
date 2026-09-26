// Port of utils/lazy_backtrack.ml (TYPECHECKER.md): lazy values whose
// forcing can be logged and undone.
#pragma once

#include <exception>
#include <functional>
#include <variant>

#include "cppcaml/typing/zone.hpp"

namespace cppcaml::typing {

template <class A, class B>
struct LazyBacktrack {  // ('a,'b) t = ('a,'b) eval ref
  enum class Kind : std::uint8_t { Done, Raise, Thunk };
  Kind kind;
  A thunk{};                // Thunk
  B done{};                 // Done
  std::exception_ptr exn;   // Raise

  static LazyBacktrack* create(A x) { return make<LazyBacktrack>(Kind::Thunk, x); }
  static LazyBacktrack* create_forced(B y) {
    auto* t = make<LazyBacktrack>(Kind::Done);
    t->done = y;
    return t;
  }

  B force(const std::function<B(A)>& f) {
    switch (kind) {
      case Kind::Done: return done;
      case Kind::Raise: std::rethrow_exception(exn);
      case Kind::Thunk: break;
    }
    try {
      B y = f(thunk);
      kind = Kind::Done;
      done = y;
      return y;
    } catch (...) {
      kind = Kind::Raise;
      exn = std::current_exception();
      throw;
    }
  }
  // get_arg: Thunk a -> Some a
  const A* get_arg() const { return kind == Kind::Thunk ? &thunk : nullptr; }
  // get_contents: Left a (a thunk) | Right b (a value); re-raises a failure
  bool is_thunk() const {
    if (kind == Kind::Raise) std::rethrow_exception(exn);
    return kind == Kind::Thunk;
  }
};

// The undo log of force_logged: every failed (Error) forcing it recorded
// is reset to its thunk by backtrack.
struct LazyLog {
  std::vector<std::function<void()>> undo;  // newest last
};

// force_logged log f x: for B = a result type -- `is_error(b)` tells Error.
template <class A, class B>
B force_logged(LazyLog& log, const std::function<B(A)>& f, LazyBacktrack<A, B>* x,
               const std::function<bool(const B&)>& is_error) {
  using K = typename LazyBacktrack<A, B>::Kind;
  switch (x->kind) {
    case K::Done: return x->done;
    case K::Raise: std::rethrow_exception(x->exn);
    case K::Thunk: break;
  }
  A e = x->thunk;
  try {
    B res = f(e);
    x->kind = K::Done;
    x->done = res;
    if (is_error(res)) log.undo.push_back([x, e] { x->kind = K::Thunk; x->thunk = e; });
    return res;
  } catch (...) {
    x->kind = K::Raise;
    x->exn = std::current_exception();
    throw;
  }
}

inline void backtrack(LazyLog& log) {
  for (auto it = log.undo.rbegin(); it != log.undo.rend(); ++it) (*it)();
}

}  // namespace cppcaml::typing
