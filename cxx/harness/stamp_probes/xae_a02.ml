module type T = sig type t val mk : unit -> t end
module F (X : T) = struct type t = { x : X.t } [@@unboxed] let mk () =
  { x = X.mk () } end
module B = struct type t = float let mk () = 0. end
module M = F(B)
let f () = M.mk ()
