module F (X : sig end) = struct type t = A let mk () = A end
module B = struct end
module M = F(B)
let f () = M.mk ()
let g () = M.mk ()
