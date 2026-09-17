module X = struct module type S = sig type t end end
module M = struct type t = (module X.S with type t = unit) end
