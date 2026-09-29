module X=struct
  module F(Y:sig type t=int val x:t end) = struct type t=Y.t let x=Y.x end
end
module DUMMY=struct type t=int let x=2 end
type u = X.F(DUMMY).t
