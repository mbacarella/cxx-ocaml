module type T = sig type t val mk : unit -> t end
module F (X : T) = struct type t = { x : X.t } let mk () = { x = X.mk () } end
module B = struct type t = float let mk () = 0. end
module F10 (Y : T) = F(F(Y))
module M = F10(B)
let f () = ()
