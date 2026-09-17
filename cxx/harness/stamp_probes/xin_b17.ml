module A = struct module Make (M : sig val x : int end) : sig
type t = private { a : int } end = struct type t = { a : int } end end
