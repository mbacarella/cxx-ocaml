module type S = sig
  module F : sig module G : functor (_ : sig end) -> sig type t end end
 end
let x = (module struct
    module F = struct module G (_ : sig end) = struct type t end end
  end : S)
