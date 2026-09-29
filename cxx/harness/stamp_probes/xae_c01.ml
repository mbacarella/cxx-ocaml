module type T = sig type t val mk : unit -> t end
module F (X : T) (Y : T) = struct type t = { x : X.t; y : Y.t } let mk () =
  { x = X.mk (); y = Y.mk () } end
module B = struct type t = float let mk () = 0. end
module C = struct type t = int let mk () = 0 end
module M = F(B)(C)
let f () = M.mk ()
