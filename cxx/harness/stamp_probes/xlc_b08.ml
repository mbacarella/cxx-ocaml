module X=struct
  module type SIG=sig type t=int val x:t module M : sig type s val y : s end
    end
  module F(Y:SIG) = struct type t=Y.t let x=Y.x end
end
module DUMMY=struct type t=int let x=2 module M = struct type s let y : s =
  assert false end end
