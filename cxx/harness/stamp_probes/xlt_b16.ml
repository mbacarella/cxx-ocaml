module F (X :sig end ) = struct module M = X module L = X end
module N = F(struct end)
include N
