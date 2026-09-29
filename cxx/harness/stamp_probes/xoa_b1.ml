module F (X : sig type t end) = struct let f (x : X.t) = x end
module P = struct type t = int end
open F(P)
