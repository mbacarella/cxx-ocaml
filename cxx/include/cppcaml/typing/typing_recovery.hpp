// Port of typing/typing_recovery.ml.  Batch ocamlc never installs an error
// log (`ref_errors` stays None, -typing-recovery is off), so logging an
// error raises it.
#pragma once

namespace cppcaml::typing::typing_recovery {

template <class E>
[[noreturn]] void log_and_raise(const E& e) {
  throw e;
}
template <class E>
[[noreturn]] void log_or_raise(const E& e) {
  throw e;
}

}  // namespace cppcaml::typing::typing_recovery
