module Make(P1 : sig type t end) : sig type t end = struct type t = int end
let _ = Int.zero
module X = Make(Int)
