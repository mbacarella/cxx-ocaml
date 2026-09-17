module type S = sig
  module F : functor (_ : sig end ) -> sig end
 end
module X = (struct
    module F (_ : sig end) = struct end
  end : S)
