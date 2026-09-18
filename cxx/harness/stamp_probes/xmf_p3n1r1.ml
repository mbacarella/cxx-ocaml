module type I = sig type t end
module Make(P1 : I)(P2 : I)(P3 : I) : sig type t1 end = struct type t1 = int end
module X = Make(Int)
