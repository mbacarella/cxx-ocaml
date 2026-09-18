module type I = sig type t end
module Make(P1 : I)(P2 : I) : sig type t end = struct type t = int end
module X = Make(Int)(Int)
type u = X.t
