module F (X :sig end ) = struct module M = struct end end
module N = F(struct end)
include N
include N
