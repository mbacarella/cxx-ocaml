module F (X : sig type t end) = struct
  type u = U : X.t -> u
  let f (x : u) = x
end
module P = struct type t = int end
open F(P)
let g = U 1
