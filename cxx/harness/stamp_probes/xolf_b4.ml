module F (X : sig type t end) = struct let id (x : X.t) = x end
module P = struct type t = int end
module M = struct module N = F(P) let g = N.id end
let q = M.g
