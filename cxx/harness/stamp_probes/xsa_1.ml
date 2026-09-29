module F (X : sig type t val z : t end) = struct let get () = X.z let pair (a : X.t) = (a, X.z) end
module M = F(struct type t = string let z = "" end)
let g = M.get
let h = M.pair
