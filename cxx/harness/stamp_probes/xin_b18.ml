module A = struct module Make (M : sig val x : int end) : sig
type t = private A | B end = struct type t = A | B end end
