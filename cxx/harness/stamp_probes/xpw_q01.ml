module X = struct module Y = struct module type S = sig type t end end end
module Y = X.Y
