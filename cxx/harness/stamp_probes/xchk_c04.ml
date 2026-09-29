module type S = sig
  module F : functor (S : sig end ) -> sig end
 end
module App (X : S) (Y : S) = struct end
module Y = App (struct module F (S : sig end) = struct end end)
    (struct module F (S : sig end) = struct end end)
