type t = bool
module F (X : sig type t end) = struct let id (x : X.t) (y : t) = (x, y) end
module M = F(struct type t = int end)
let g = M.id
