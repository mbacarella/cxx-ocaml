module F (X : sig type t end) = struct let id (x : X.t option) = x end
module M = F(struct type t = int -> int end)
let g = M.id
