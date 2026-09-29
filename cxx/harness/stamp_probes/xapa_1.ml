module type S = sig type t end
module G (X : S) = struct type t = X.t type u end
module M0 = struct type t end
module F (X : S) = struct type v = X.t end
module A = F(G(M0))
