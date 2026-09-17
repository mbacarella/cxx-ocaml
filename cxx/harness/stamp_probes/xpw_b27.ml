module X = struct module type S = sig type t end end
type u = (module X.S with type t = unit) * (module X.S with type t = int)
