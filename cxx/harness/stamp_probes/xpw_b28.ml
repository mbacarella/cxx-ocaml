module X = struct module type S = sig type t end end
type u = (module X.S with type t = unit)
type v = (module X.S with type t = unit)
type w = (module X.S with type t = unit)
