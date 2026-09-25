module F (X : sig type t end) = struct
  module N = struct type n = X.t option end let h (x : N.n) = x end
module P = struct type t = string end
open F(P)
let k = h
