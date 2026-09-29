module F (X : sig type t end) = struct type u = U of X.t | V end
module P = struct type t = int end
open F(P)
let c x = match x with U y -> y | V -> 0
