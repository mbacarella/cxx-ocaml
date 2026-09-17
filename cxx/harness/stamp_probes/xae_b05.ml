module type T = sig type t val mk : unit -> t end
module F (X : T) = struct type t = { x : X.t } let mk () = { x = X.mk () } end
module B = struct type t = float let mk () = 0. end
module C = struct type t = int let mk () = 0 end
module M1 = F(B)
module M2 = F(C)
let g () = M2.mk ()
