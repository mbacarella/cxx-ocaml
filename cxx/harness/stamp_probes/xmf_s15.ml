module type I = sig type t end
module Make(P1 : I)(_ : I) : sig type t end = struct type t = int end
let _ = Int.zero
module X = Make(Int)(Int)
