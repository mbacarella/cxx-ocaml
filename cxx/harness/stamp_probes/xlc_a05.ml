module type SIG=sig type t=int val x:t end
module F(Y:SIG) : SIG = struct type t=Y.t let x=Y.x end
module DUMMY=struct type t=int let x=2 end
let x = (3 : F(DUMMY).t)
