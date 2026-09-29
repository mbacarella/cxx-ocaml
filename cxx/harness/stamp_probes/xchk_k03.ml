module X : sig
  module F : functor (S : sig end ) -> sig type t end
 end = struct
    module F (S : sig end) = struct type t end
  end
module Arg = struct end
module FArg = X.F (Arg)
