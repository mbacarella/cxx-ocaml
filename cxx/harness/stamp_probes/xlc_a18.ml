module X=struct
  module type SIG=sig type t=int val x:t end
  module F(Y:SIG) = struct type t=Y.t let x=Y.x end
  module G(Y:SIG) = struct type t=Y.t let x=Y.x end
end
module DUMMY=struct type t=int let x=2 end
type u = X.F(DUMMY).t
type w = X.G(DUMMY).t
