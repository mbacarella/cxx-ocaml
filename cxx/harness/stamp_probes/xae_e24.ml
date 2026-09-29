module type T = sig type t val mk : unit -> t end
module P = struct module F (X : T) = struct type t =
  { x : X.t } let mk () = { x = X.mk () } end end
module B = struct type t = float let mk () = 0. end
module M = P.F(B)
let f () = M.mk ()
