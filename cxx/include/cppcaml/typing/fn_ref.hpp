// A non-owning reference to a callable, for the parameters of higher-order
// functions that only call their argument during the call (OCaml's `iter f
// ty`, `with_... f`): passing a capturing lambda to a `const std::function&`
// parameter heap-allocates it (past libstdc++'s 16-byte buffer), and the
// type checker's traversals do that millions of times per unit.  A FnRef
// must not outlive the callable it was built from.
#pragma once

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace cppcaml::typing {

template <class Sig>
class FnRef;

template <class R, class... A>
class FnRef<R(A...)> {
 public:
  template <class F, class = std::enable_if_t<!std::is_same_v<std::decay_t<F>, FnRef> &&
                                              std::is_invocable_r_v<R, F&, A...>>>
  FnRef(F&& f) noexcept  // NOLINT(google-explicit-constructor)
      : obj_(const_cast<void*>(static_cast<const void*>(std::addressof(f)))),
        call_([](void* o, A... a) -> R {
          return std::invoke(*static_cast<std::remove_reference_t<F>*>(o), std::forward<A>(a)...);
        }) {}
  // a plain function (a function name decays to a pointer, kept by value)
  FnRef(R (*fp)(A...)) noexcept  // NOLINT(google-explicit-constructor)
      : obj_(reinterpret_cast<void*>(fp)),
        call_([](void* o, A... a) -> R { return reinterpret_cast<R (*)(A...)>(o)(std::forward<A>(a)...); }) {}

  R operator()(A... a) const { return call_(obj_, std::forward<A>(a)...); }

 private:
  void* obj_;
  R (*call_)(void*, A...);
};

}  // namespace cppcaml::typing
