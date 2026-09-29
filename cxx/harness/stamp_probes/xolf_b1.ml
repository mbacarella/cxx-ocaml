module F (X : sig type t end) = struct let id (x : X.t) = x end
open F(struct type t = int end)
let g = id
