module F (X : sig end) = struct type t = int let mk () = 1 end
module B = struct end
module M = F(B)
let f () = M.mk ()
