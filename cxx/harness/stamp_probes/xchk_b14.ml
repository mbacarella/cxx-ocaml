module type S = sig
  module F : sig module G : functor () -> sig end end
 end
module X = (struct
    module F = struct module G () = struct end end
  end : S)
