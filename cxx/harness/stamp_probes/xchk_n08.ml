module type S = sig
  module F : sig module G : sig module H : functor (S : sig end) -> sig end
    end end
 end
let x = (module struct
    module F = struct module G = struct module H (S : sig end) = struct end
      end end
  end : S)
