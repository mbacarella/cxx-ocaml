module type T = sig type t val mk : unit -> t end
module F (X : T) = struct type t = { x : X.t } let mk () = { x =
  X.mk () } let v = { x = X.mk () } let use (v : t) = ignore v end
module B = struct type t = float let mk () = 0. end
module M = F(B)
let f () = M.use { M.x = 0. }
