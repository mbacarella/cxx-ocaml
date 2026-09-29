module type S = sig
  module F : sig module G : sig type t end -> sig end end
 end
let x = (module struct
    module F = struct module G (_ : sig type t end) = struct end end
  end : S)
