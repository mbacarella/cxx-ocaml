module X = struct module type S = sig type t end end
module type S2 = X.S
type t = (module S2 with type t = unit)
