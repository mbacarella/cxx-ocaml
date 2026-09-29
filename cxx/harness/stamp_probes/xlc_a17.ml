module X=struct
  module type SIG=sig type t=int val x:t end
  module F(Y:SIG)(Z:SIG) = struct type t=Y.t let x=Z.x end
end
module DUMMY=struct type t=int let x=2 end
type u = X.F(DUMMY)(DUMMY).t
