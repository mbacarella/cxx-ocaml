module X = struct module type S = sig type t end end
module Y = X
type t = (module Y.S with type t = unit)
type u = (module X.S with type t = unit)
