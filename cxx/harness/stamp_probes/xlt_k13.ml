module F (X :sig end ) = struct type t let v = 1 end
module N = F(struct end)
include N
type t = int
