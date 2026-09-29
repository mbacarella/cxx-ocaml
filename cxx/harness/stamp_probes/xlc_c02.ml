module X=struct
  module type SIG=sig type t val x:t end
  module F(Y:SIG with type t = int) = struct type t=Y.t let x=Y.x end
end
module DUMMY=struct type t=int let x=2 end
module N = X.F(DUMMY)
