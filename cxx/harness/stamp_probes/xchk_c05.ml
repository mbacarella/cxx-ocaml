module type S = sig
  module F : functor (S : sig end ) -> sig end
 end
module App (X : S) = struct end
module Z = struct module F (S : sig end) = struct end end
module Y = App (Z)
module Y2 = App (Z)
