module type I = sig type t end
module Make(P1 : I)(P2 : I) = struct let x = 1 end
module X = Make(Int)(Int)
