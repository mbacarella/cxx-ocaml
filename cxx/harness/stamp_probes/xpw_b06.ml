module X = struct module Y = struct module type S = sig type t end end end
type t = (module X.Y.S with type t = unit)
