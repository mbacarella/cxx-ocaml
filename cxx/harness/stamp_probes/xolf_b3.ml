module F (X : sig type t end) = struct let id (x : X.t) = x end
module P = struct type t = int end
module M = struct open F(P) let g = id end
let q = M.g
