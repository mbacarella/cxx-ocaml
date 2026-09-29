module G (X : sig type t end) = struct type u = A of X.t end
module F = G
module M = F (Int)
