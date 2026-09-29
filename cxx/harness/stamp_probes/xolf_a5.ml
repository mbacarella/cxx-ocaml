module F (X : sig type t end) = struct
  type u = { f : X.t } let mk x = { f = x } end
module P = struct type t = char end
open F(P)
let m = mk
