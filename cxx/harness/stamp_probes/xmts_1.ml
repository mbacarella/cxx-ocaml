module type S = sig module type T module X : T end
module M = struct module type T = sig type t end end
module F (X : S) = X.X
