module type S = sig val zero : int val one : int val add : int -> int -> int end
let impl : (module S) = (module Int : S)
module M : sig val zero : int end = struct include (val impl : S) end
