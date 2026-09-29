module type T = sig type t val mk : unit -> t end
module F (X : T) = struct type t = { x : X.t } [@@unboxed] let mk () =
  { x = X.mk () } end
module F10 (X : T) = F(F(F(F(F(F(F(F(F(F(X))))))))))
module F100 (X : T) = F10(F10(F10(F10(F10(F10(F10(F10(F10(F10(X))))))))))
module B = struct type t = float let mk () = 0. end
module M = F(F100(B))
let run () = let x = M.mk () in ignore x
