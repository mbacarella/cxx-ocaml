module type I = sig type t end
module Make(P1 : I)(P2 : I) : sig type t end = struct type t = int end
module A = struct type t = int end
module B = struct type t = int end
let _ = Int.zero
module X = Make(Int)
