module F (X : sig type t val v : t end) = struct let g = X.v end
module B = struct type t = int let v = 3 end
module M = F(B)
let h = M.g
