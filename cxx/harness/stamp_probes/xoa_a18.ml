module F (X : sig type t end) = struct
  type u = U of X.t
  type w = W
  let f (x : u) = x
end
module P = struct type t = int end
open F(P)
let g = W
