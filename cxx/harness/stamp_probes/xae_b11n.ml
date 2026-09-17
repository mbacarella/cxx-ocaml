module type T = sig type t val mk : unit -> t end
module F (X : T) = struct type t = X.t let mk () = X.mk () end
module G (X : T) = struct type t = X.t let mk () = X.mk () end
module B = struct type t = float let mk () = 0. end
module M = F(G(B))
