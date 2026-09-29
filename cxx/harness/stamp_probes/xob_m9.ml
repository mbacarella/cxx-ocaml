module F (X : sig type t end) = struct type u = U of X.t let f (x : u) = x end
module P = struct type t = int end
open F(P)
type w = u
let _ = U 1
