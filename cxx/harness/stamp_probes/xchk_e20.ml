module type S = sig
  module M : sig val s : unit end
  module F : functor (S : sig end ) -> sig type t end
 end
module X = struct
    module M = struct let s = () end
    module F (S : sig end) = struct type t end
  end
module Y = X.M
