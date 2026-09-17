module type S = sig
  module F : sig module G : functor (_ : sig end) -> sig end end
 end
module X : S = struct
    module F = struct module G (_ : sig end) = struct end end
  end
