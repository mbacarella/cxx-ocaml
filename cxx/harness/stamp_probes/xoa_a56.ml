module F (X : sig type t end) = struct
  type 'a u = U : 'a -> 'a u | V : X.t -> int u
  let f (x : 'a u) = x
end
module P = struct type t = int end
open F(P)
let h = f
let g = ignore (U 1)
