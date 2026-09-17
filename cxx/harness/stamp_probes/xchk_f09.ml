module type S = sig
  module F : functor (S : sig end ) -> sig type t val v : t end
 end
module X : S = struct
    module F (S : sig end) = struct type t let v = assert false end
  end
module Arg = struct end
module FArg = X.F (Arg)
let _ = FArg.v
