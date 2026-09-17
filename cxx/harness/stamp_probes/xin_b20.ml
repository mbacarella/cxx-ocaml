module A = struct module Make (M : sig val x : int end) : sig
module N : sig type t = private string end end = struct
module N = struct type t = string end end end
