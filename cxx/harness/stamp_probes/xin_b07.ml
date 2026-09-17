module A = struct module Make (M : sig val x : int end) : sig
type t = private string type u = private int end = struct type t = string
type u = int end end
