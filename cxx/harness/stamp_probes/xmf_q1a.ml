module type I = sig type t end
module Make(P1 : I) : sig val x : int end = struct let x = 1 end
