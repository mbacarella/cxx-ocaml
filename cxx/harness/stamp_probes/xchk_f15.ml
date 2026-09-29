module type S = sig
  module F : functor (S : sig end ) -> sig type t end
 end
module X : S = struct
    module F (S : sig end) = struct type t end
  end
module Arg = struct end
module M = struct
  module FArg = X.F (Arg)
  type u = FArg.t
end
