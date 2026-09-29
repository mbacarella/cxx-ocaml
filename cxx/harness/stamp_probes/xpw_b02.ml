module X = struct module type S = sig type t type u end end
type t = (module X.S with type t = unit)
