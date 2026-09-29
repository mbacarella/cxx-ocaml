module X2=struct
  module type SIG=sig type t=int val x:t end
  module F(Y:SIG)(Z:SIG) = struct
    type t=Y.t
    let x=Y.x
    type t'=Z.t
    let x'=Z.x
  end
end;;
module DUMMY=struct type t=int let x=2 end;;
let x = (3 : X2.F(DUMMY)(DUMMY).t);;
let x = (3 : X2.F(DUMMY)(DUMMY).t');;
