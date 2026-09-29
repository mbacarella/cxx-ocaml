module type T = sig module G : functor (S : sig end ) -> sig end end
module type S = sig
  module F : T
 end
let x = (module struct
    module F : T = struct module G (S : sig end) = struct end end
  end : S)
