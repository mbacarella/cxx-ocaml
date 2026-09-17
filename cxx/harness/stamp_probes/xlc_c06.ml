module type SIG=sig type t val x:t end
module F(Y:SIG with type t := int) = struct type t=int let x=Y.x end
module DUMMY=struct let x = 3 end
type u = F(DUMMY).t
