module A = struct module Make (M : sig val x : int end) : sig
type t = private int end = struct type t = int end end
