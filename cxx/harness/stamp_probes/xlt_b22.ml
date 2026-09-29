module F (X :sig type t end ) = struct type u = X.t end
module A = struct type t end
module N = F(A)
include N
