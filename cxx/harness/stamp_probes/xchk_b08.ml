module type S = sig
  module F : functor (S : sig end) (T : sig end) -> sig end
 end
module X = (struct
    module F (S : sig end) (T : sig end) = struct end
  end : S)
