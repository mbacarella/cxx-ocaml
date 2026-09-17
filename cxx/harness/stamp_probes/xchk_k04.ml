module type S = sig
  module F : functor (S : sig end ) -> sig end
 end
module Ap (X : sig module F (_ : sig end) : sig end end) = struct
  let x = (module X : S)
end
