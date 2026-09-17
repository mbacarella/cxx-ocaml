module X=struct module type SIG=sig type t=int val x:t end end
module F(Y:X.SIG) = struct type t=Y.t let x=Y.x end
module DUMMY=struct type t=int let x=2 end
type u = F(DUMMY).t
