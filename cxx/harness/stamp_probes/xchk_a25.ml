module type S = sig
  module F : functor (S : sig end ) -> sig end
  type t
 end
let x = (module (struct
    module F (S : sig end) = struct end
    type t
  end : S) : S)
