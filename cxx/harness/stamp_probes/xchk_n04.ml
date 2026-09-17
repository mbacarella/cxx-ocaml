module type S = sig
  module F : sig module G : functor (S : sig end) (T : sig end) -> sig end end
 end
let x = (module struct
    module F = struct module G (S : sig end) (T : sig end) = struct end end
  end : S)
