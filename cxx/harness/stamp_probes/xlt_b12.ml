module F (X :sig end ) = struct module M = X end
include F(struct end)
