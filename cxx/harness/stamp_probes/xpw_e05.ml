module X = struct module Y = struct module type S = sig type t end end end
module Y = X.Y
type t = (module Y.S with type t = unit)
type u = (module Y.S with type t = int)
