module type I = sig type t end
module Make(P1 : I)(P2 : I) : sig type t val x : int end = struct type t = int
  let x = 1 end
module X = Make(Int)(Int)
