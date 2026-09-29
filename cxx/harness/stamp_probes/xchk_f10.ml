module type S = sig
  module F : functor (S : sig end ) -> sig type t end
 end
module X : S = struct
    module F (S : sig end) = struct type t end
  end
module FArg = X.F (struct end)
type u = FArg.t
