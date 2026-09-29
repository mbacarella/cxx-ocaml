module F (X : sig end) = struct type t = A let mk () = A end
module M = F(struct end)
let f () = M.mk ()
