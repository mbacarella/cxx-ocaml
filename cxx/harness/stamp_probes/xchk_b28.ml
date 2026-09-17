module type S = sig
  module F : sig module G : functor (S : sig end) -> sig end end
 end
module X = (struct
    module F = struct module G (S : sig end) = struct end end
  end : S)
