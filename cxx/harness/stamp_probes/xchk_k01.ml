module type S = sig
  module M : sig val s : unit end
  module F : functor (S : sig end ) -> sig type t end
 end
module X = struct
    module M = struct let s = () end
    module F (S : sig end) = struct type t end
  end
module Arg = struct end
module FArg = X.F (Arg)
type u = FArg.t
