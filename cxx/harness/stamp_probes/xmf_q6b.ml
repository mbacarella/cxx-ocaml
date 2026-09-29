module type I = sig type t end
module Make(P1 : I)(P2 : I) : sig type t val x : P1.t end = struct
  type t = int let x = Obj.magic 0 end
module X = Make(Int)(Int)
