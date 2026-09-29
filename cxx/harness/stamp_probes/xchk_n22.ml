module type S = sig
  module F : functor (S : sig end ) -> sig module G : functor () -> sig end
    end
 end
let x = (module struct
    module F (S : sig end) = struct module G () = struct end end
  end : S)
