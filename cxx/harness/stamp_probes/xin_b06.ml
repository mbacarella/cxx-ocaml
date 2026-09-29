module A = struct module Make (M : sig val x : int end) : sig
type t = private string end = struct type t = private string end end
