module F (X : sig type t end) = struct type u = A of X.t end
module M = F (Int)
include M
