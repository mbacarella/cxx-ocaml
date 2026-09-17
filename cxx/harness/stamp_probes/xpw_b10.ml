module X = struct module type S = sig type t end end
type t = (module X.S with type t = unit)
type u = (module X.S)
