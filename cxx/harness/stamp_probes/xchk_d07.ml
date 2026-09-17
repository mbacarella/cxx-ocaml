module type S = sig
  module F : functor (S : sig end ) -> sig end
 end
module X = struct
    module N = struct module F (_ : sig end) = struct end end
  end
module Y = (X.N : S)
