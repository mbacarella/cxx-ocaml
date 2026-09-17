module type S = sig
  module F : functor (S : sig end ) -> sig type t end
 end
module X : S = struct
    module F (S : sig end) = struct type t end
  end
module Arg = struct end
module Arg2 = struct end
module FArg = X.F (Arg)
module FArg2 = X.F (Arg2)
type u = FArg.t
type v = FArg2.t
