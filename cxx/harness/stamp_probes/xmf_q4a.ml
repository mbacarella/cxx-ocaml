module type I = sig type t end
module Make(P1 : I)(P2 : I) = struct type t = int end
