module X = struct module type S = sig type t end end
type t = { a : (module X.S with type t = unit) }
