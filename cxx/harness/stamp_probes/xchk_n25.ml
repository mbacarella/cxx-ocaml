module type S = sig
  module F : functor (S : sig end ) -> sig end
  module type T = sig module G : functor (S : sig end ) -> sig end end
 end
let x = (module struct
    module F (S : sig end) = struct end
    module type T = sig module G : functor (S : sig end ) -> sig end end
  end : S)
