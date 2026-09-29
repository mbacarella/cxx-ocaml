module type T = sig type t val mk : unit -> t end
module F (X : T) = struct type t = X.t let mk () = X.mk () end
module G (X : T) = struct type t = { y : X.t } let mk () = { y = X.mk () } end
module B = struct type t = float let mk () = 0. end
module F10 (Y : T) = F(F(Y))
module M = F10(G(B))
let f () = ignore (M.mk ())
