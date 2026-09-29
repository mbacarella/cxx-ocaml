module X = struct module Y = struct module type S = sig type t end type u let
  v = 1 module N = struct type w end end end
module Y = X.Y
module Z = X.Y
type t = Z.u
type t2 = Y.u
