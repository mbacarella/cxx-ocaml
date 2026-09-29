module type S = sig val zero : int module N : sig val a : int val b : int end
  module type T = sig val c : int end end
let impl : (module S) = (module struct let zero = 0 module N = struct
  let a = 1 let b = 2 end module type T = sig val c : int end end : S)
include (val impl : S)
