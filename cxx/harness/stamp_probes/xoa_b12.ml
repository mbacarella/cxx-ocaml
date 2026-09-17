module F (X : sig type t end) = struct type u = X.t list let f (x : u) = x end
module P = struct type t = int end
module G (Y : sig type t end) = F(Y)
open G(P)
