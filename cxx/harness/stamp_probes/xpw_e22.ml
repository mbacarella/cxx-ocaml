module X = struct module Y = struct type u end end
module Z = struct module Y = X.Y type t = Y.u end
