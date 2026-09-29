module F (X : sig type t end) = struct type u = U of X.t
  let mk x = U x end
module B = struct type t = string end
module M = F(B)
let f = M.mk
