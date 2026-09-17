module type T = sig type t val mk : unit -> t end
module F (X : T) = struct type t = { x : X.t } let mk () = { x =
  X.mk () } module N = struct type u = { z : int } let mku () = { z =
  0 } end end
module B = struct type t = float let mk () = 0. end
module M = F(B)
let f () = M.N.mku ()
