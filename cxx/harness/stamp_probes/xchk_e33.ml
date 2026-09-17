module type S = sig
  module M : sig val s : unit end
  module F : functor (S : sig end ) -> sig type t end
 end
module X : S = struct
    module M = struct let s = () end
    module F (_ : sig end) = struct type t end
  end
module Arg = struct end
let _ = let module FArg = X.F (Arg) in ()
