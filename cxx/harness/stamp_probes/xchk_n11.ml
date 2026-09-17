module type S = sig
  module F : sig module G : functor (_ : sig end) -> sig end end
 end
let x = (module struct
    module F = struct module G (S : sig end) = struct end end
  end : S)
