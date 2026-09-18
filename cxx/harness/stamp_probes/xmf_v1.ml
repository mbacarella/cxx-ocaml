module type S = sig val zero : int val one : int val add : int -> int -> int end
let impl : (module S) = (module Int : S)
include (val impl : S)
