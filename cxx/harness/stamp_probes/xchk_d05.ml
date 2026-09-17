module type S = sig
  module F : functor (S : sig end ) -> sig end
 end
module X = struct
    module F (_ : sig end) = struct end
  end
module Y = (X : S)
module Z = (Y : S)
