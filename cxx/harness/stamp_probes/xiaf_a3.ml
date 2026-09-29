module F (X : sig end) = struct type t = int let v = 1 end
module N = F(struct end)
include N
let w = v
