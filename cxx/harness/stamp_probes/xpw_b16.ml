module X = struct module type S = sig type t end end
type t = A of (module X.S with type t = unit)
