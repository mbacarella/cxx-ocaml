module F (X : sig type t end) = struct
  type 'a u = U : 'a -> 'a u and w = W of int u end
module P = struct type t = int end
open F(P)
let g (W x) = x
