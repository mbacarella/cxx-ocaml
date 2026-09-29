module type S = sig
  module F : functor (S : sig end ) -> sig type t type w end
 end
module X : S = struct
    module F (S : sig end) = struct type t type w end
  end
module Arg = struct end
module FArg = X.F (Arg)
type u = FArg.t
