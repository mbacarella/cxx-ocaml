module type S = sig
  module F : functor (S : sig end ) (T : sig end) -> sig end
 end
module X = (struct
    module F (_ : sig end) (_ : sig end) = struct end
  end : S)
