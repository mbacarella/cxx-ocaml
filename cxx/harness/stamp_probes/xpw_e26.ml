module X = struct module Y = struct module type S = sig type t end type u let
  v = 1 module N = struct type w type w2 module Q = struct type q end end end
  end
module Y = X.Y
type t = Y.N.Q.q
