module F (X : sig end) = struct let mk x = x end
module B = struct end
module M = F(B)
let f = M.mk
