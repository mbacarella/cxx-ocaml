module type SIG=sig type t val x:t end
module F(Y:SIG with type t = int) = struct type t=Y.t let x=Y.x end
module DUMMY=struct type t=int let x=2 end
type u = F(DUMMY).t
