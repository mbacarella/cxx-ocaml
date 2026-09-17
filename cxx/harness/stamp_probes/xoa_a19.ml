module F (X : sig type t val v : t end) = struct
  type u = U of X.t
  let f (x : u) = x
end
module P = struct type t = int let v = 1 end
open F(P)
let g = U 1
