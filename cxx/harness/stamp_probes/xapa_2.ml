module type S = sig type t end
module G (X : S) = struct type t = X.t end
module M0 = struct type t end
module F (X : S) = struct type w type v = X.t end
module A = F(G(M0))
module B = F(G(struct type t end))
