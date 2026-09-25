module F (X : sig type t val v : t end) = struct let get () = X.v end
module P = struct type t = int let v = 3 end
open F(P)
let g () = get ()
