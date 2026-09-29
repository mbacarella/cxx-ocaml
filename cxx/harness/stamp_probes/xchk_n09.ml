module type S = sig
  module F : sig module G : sig module H : functor () -> sig end end end
 end
let x = (module struct
    module F = struct module G = struct module H () = struct end end end
  end : S)
