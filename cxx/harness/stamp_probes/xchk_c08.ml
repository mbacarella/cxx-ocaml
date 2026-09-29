module type S = sig
  module F : functor (S : sig end ) -> sig end
 end
module App (X : S) = struct end
module P = struct
  module Y = App (struct module F (_ : sig end) = struct end end)
end
