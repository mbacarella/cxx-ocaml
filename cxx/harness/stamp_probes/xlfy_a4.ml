module F (X : sig type t val v : t end) = struct let g = X.v end
module B = struct type t = A let v = A end
module M = F(B)
let h = M.g
