module X = struct module type S = sig type t end end
module Z = struct type u = (module X.S) end
