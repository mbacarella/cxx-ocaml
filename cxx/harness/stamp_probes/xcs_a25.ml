module F (X : sig type t end) = struct type u = A of X.t end
include F (Int)
