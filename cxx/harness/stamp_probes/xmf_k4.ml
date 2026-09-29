module type I = sig type t end
module Make(A : I)(B : I) : sig type t end = struct type t = int end
module X = Make(Int)
