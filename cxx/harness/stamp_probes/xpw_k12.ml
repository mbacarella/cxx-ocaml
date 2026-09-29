module X = struct module Y = struct type u end end
module Z = struct module Y = X.Y end
type t = Z.Y.u
