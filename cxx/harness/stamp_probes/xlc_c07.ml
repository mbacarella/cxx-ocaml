module type SIG=sig type t val x:t end
module type SIG2 = SIG with type t = int
module F(Y:SIG2) = struct type t=int let x=Y.x end
module DUMMY=struct type t=int let x=2 end
type u = F(DUMMY).t
