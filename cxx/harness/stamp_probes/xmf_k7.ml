module type I = sig type t end
module Make(A : I)(B : I) : sig type t val x : A.t end = struct type t = int
  let x = Obj.magic 0 end
module X = Make(Int)(Int64)
