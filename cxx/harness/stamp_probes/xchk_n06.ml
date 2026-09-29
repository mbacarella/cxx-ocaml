module type S = sig
  module F : sig module G : functor () -> sig end end
 end
let x = (module struct
    module F = struct module G () = struct end end
  end : S)
