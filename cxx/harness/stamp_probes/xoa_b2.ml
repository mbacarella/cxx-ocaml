module F (X : sig type t end) = struct type u = X.t list let f (x : u) = x end
module P = struct type t = int end
module G (Y : sig type u end) = struct type v = Y.u option end
open G(F(P))
