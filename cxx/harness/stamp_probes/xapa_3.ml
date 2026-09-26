module type S = sig type t end
module G (X : S) (Y : S) = struct type t = X.t * Y.t end
module M0 = struct type t = int end
module F (X : S) = struct type v = X.t let f (x : v) = x end
module A = F(G(M0)(M0))
