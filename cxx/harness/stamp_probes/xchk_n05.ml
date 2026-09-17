module type S = sig
  module F : sig module G : functor (S : sig type t end) -> sig end end
 end
let x = (module struct
    module F = struct module G (S : sig type t end) = struct end end
  end : S)
