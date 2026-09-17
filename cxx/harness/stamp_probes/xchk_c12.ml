module type S = sig
  module F : functor (S : sig end ) -> sig end
 end
module App (X : S) = struct end
module Y = App (struct module F (S : sig end) = struct end end)
module Z = App (struct module F (_ : sig end) = struct end end)
