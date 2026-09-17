module type S = sig
  module F : sig module G : sig type t end end
 end
let x = (module struct
    module F = struct module G = struct type t end end
  end : S)
