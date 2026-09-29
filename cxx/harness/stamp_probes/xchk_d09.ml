module type S = sig
  module F : functor (S : sig end ) -> sig end
 end
module P = struct
  module X = struct module F (_ : sig end) = struct end end
end
let x = (module P.X : S)
