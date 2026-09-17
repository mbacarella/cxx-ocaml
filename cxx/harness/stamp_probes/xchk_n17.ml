module type S = sig
  module F : sig module G : functor (S : sig end) -> sig type t end end
 end
let x = (module struct
    module F = struct module G (S : sig end) = struct type t end end
  end : S)
