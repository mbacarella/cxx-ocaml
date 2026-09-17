module type S = sig
  module F : functor (_ : sig end) (_ : sig end) -> sig end
 end
let x = (module struct
    module F (_ : sig end) (_ : sig end) = struct end
  end : S)
