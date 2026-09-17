module F (X :sig end ) = struct module M = X end
module N = F(struct end)
module P = struct include N end
