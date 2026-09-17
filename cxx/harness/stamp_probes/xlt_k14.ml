module F (X :sig end ) = struct type t end
module A = struct end
module N = F(A)
include N
include N
