module F (X : sig type t end) = struct type u = X.t list let f (x : u) = x end
module P = struct type t = int end
module type S = sig type u val f : u -> u end
module G (X : sig type t end) : S = F(X)
open struct type z = int end
module Q = G(P)
