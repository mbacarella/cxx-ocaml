module F (X :sig end ) = struct type t end
module N = F(struct end)
module P = struct include N include N end
