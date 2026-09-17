module type S = sig
  module F : functor (S : sig end ) -> sig type t module N : sig type w end
    end
 end
module X : S = struct
    module F (S : sig end) = struct type t module N = struct type w end end
  end
module Arg = struct end
module FArg = X.F (Arg)
type u = FArg.N.w
