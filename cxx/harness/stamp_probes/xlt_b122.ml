module F (X :sig end ) = struct type t end
module N = F(struct end)
include N
type u = int
