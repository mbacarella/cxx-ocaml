module type T = sig type t val mk : unit -> t end
module F (X : T) = struct type t = A of X.t | N let mk () = A (X.mk ()) end
module B = struct type t = float let mk () = 0. end
module M = F(B)
let f () = ignore M.N
