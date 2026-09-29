module F (X : sig type t end) = struct type u = U of X.t let f (x : u) = x end
module P = struct type t = int end
open F(P)
let g x = match x with U _ -> 1
