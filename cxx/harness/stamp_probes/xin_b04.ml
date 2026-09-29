module A = struct module Make (M : sig type u end) : sig
type t = private string end = struct type t = string end end
