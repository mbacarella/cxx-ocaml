module F (X :sig end ) = struct type t end
module N = F(struct end)
include N
include N
include N
