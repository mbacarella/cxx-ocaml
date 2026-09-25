module F (X : sig end) = struct type t = A let mk () = A end
module B = struct end
include F(B)
let f () = mk ()
