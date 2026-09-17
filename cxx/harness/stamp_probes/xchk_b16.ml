module type S = sig
  module F : functor (S : sig end) -> sig end
 end
module X : S = struct
    module F (S : sig end) = struct end
  end
