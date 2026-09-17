module F (X :sig end ) = struct type t end
module N = F(struct end)
module O = F(struct end)
include N
include O
