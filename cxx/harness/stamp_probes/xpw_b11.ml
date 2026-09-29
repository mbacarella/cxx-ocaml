module X = struct module type S = sig type t end end
type u = (module X.S)
type t = (module X.S with type t = unit)
